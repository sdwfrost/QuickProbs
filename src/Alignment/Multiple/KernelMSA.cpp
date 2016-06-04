#include <memory>
#include <algorithm>
#include <omp.h>

#include "Hardware/Kernel.h"
#include "KernelRepository/KernelFactory.h"
#include "Common/MemoryTools.h"
#include "Common/Log.h"
#include "Common/rank.h"
#include "Common/deterministic_random.h"

#include "SLinkTree.h"
#include "ClusterTree.h"
#include "PhylipTree.h"
#include "QuickPosteriorStage.h"
#include "QuickConsistencyStage.h"
#include "RandomRefinement.h"
#include "ColumnRefinement.h"
#include "ScoringRefinement.h"
#include "TreeRefinement.h"
#include "KernelMSA.h"
#include "ParallelProbabilisticModel.h"

#undef VERSION

using namespace std;
using namespace quickprobs;

/// <summary>
/// See declaration for all the details.
/// </summary>
KernelMSA::KernelMSA(std::shared_ptr<clex::OpenCL> cl, std::shared_ptr<Configuration> config)
	: ExtendedMSA(), cl(cl)
{
	TIMER_CREATE(timer);
	TIMER_START(timer);
	
	this->config = config;
	this->version =  std::to_string(QUICKPROBS_VERSION);

	int numCores = omp_get_num_procs();
	LOG_NORMAL << "Detected " << numCores << " CPU cores" << endl;

	if (config->hardware.numThreads <= 0){
		config->hardware.numThreads = numCores;
		LOG_NORMAL << "Automatically set " << config->hardware.numThreads << " OpenMP threads." << endl;
	} else {
		LOG_NORMAL <<"Statically set " << config->hardware.numThreads<<" OpenMP threads." << endl << endl;
	}

	// reassign stages pointer to their kernel variants if OpenCL device specified
	if (cl != nullptr) {
		LOG_NORMAL << "Configuring OpenCL for " << cl->mainDevice->info->deviceName << "." << endl;
		KernelFactory::instance(cl).setFastMath(!config->optimisation.usePreciseMath);
		posteriorStage = std::shared_ptr<PosteriorStage>(new QuickPosteriorStage(cl, config));
		LOG_MEM("After posterior creation");
		consistencyStage = std::shared_ptr<ConsistencyStage>(new QuickConsistencyStage(cl, config));
		LOG_MEM("After consistency creation");
		
	} else {
		LOG_NORMAL << "OpenCL platform and device not specified - using pure CPU version." << endl; 
		posteriorStage = std::shared_ptr<PosteriorStage>(new PosteriorStage(config));
		consistencyStage = std::shared_ptr<ConsistencyStage>(new ConsistencyStage(config));
	}
	
	constructionStage = std::shared_ptr<ConstructionStage>(new ConstructionStage(config));
	
	if (config->algorithm.refinement.type == RefinementType::Column) {
		refinementStage = std::shared_ptr<RefinementBase>(new ColumnRefinement(config, constructionStage));
	} else if (config->algorithm.refinement.type == RefinementType::Tree) {
		refinementStage = std::shared_ptr<RefinementBase>(new TreeRefinement(config, constructionStage));
	} else {
		refinementStage = std::shared_ptr<RefinementBase>(new RandomRefinement(config, constructionStage));
	}

	refinementStage->registerObserver(this);

	TIMER_STOP(timer);
	STATS_WRITE("time.0.1-initialisation", timer.seconds());
}


std::unique_ptr<quickprobs::MultiSequence> quickprobs::KernelMSA::doAlign(MultiSequence *sequences)
{
	assert (sequences);

	TIMER_CREATE(timer);
	TIMER_CREATE(totalTimer);
	TIMER_START(totalTimer);

	const int numSeqs = sequences->count();
	ISequenceSet* set;
	ContiguousMultiSequence cms(*sequences);
	set = &cms;
	
	// create distance matrix
	Array<float> distances(numSeqs);
	Array<SparseMatrixType*> sparseMatrices(numSeqs);

	// all pairwise steps are encapsulated in a separate function now
	posteriorStage->operator()(*set, distances, sparseMatrices);
	
	// create the guide tree
	std::shared_ptr<GuideTree> tree;
	if (config->algorithm.treeKind == TreeKind::Chained) {
		degenerateDistances(distances);
		tree = std::shared_ptr<GuideTree>(new ClusterTree(distances));
	} else if (config->algorithm.treeKind == TreeKind::UPGMA) {
		tree = std::shared_ptr<GuideTree>(new ClusterTree(distances));
	} else {
		tree = std::shared_ptr<GuideTree>(new SLinkTree(distances));
	}
	
	(*tree)();
	
	// perform the consistency transformation desired number of times
	auto weights = tree->getWeights();
	
	// calculate subtree distances and normalize them
	Array<float> consistencyDistances; 
	
	if (config->algorithm.consistency.mode == SelectivityMode::Subtree) {
		consistencyDistances = tree->calculateSubtreeDistances();
	} else if (config->algorithm.consistency.mode == SelectivityMode::Similarity) {
		consistencyDistances = distances;
	} else if (config->algorithm.consistency.mode == SelectivityMode::Seed) {
		consistencyDistances = Array<float>(numSeqs);
		std::fill(consistencyDistances.getData().begin(), consistencyDistances.getData().end(), std::numeric_limits<float>::max());
		
		std::vector<int> seedIds(config->algorithm.consistency.selectivity);
		std::mt19937 eng;
		det_uniform_int_distribution<int> dist(0, numSeqs - 1);
		std::generate(seedIds.begin(), seedIds.end(), [&eng, &dist]()->float {
			return dist(eng);
		});

		for (int seed : seedIds) {
			for (int i = 0; i < numSeqs; ++i) {
				consistencyDistances[seed][i] = consistencyDistances[i][seed] = 0.0;
			}
		}
	}
	
	if (config->algorithm.consistency.normalization == SelectivityNormalization::Stochastic) {
		// normalize distances for stochastic 
		float maxElem = *std::max_element(consistencyDistances.getData().begin(), consistencyDistances.getData().end());
		if (maxElem > 1.0f) {
			std::transform(consistencyDistances.getData().begin(), consistencyDistances.getData().end(), consistencyDistances.getData().begin(), 
				[maxElem](float x) -> float {
				return x / maxElem;
			});
		}
	} else if (config->algorithm.consistency.normalization == SelectivityNormalization::RankedStochastic) {
		// rank and normalize distances for ranked 
		for (int i = 0; i < distances.size(); ++i) {
			consistencyDistances[i][i] = std::numeric_limits<float>::max();
		}
			
		rank_range(
			consistencyDistances.getData().begin(), 
			consistencyDistances.getData().end(), 
			consistencyDistances.getData().begin(), 
			std::greater<float>());

		std::transform(consistencyDistances.getData().begin(), consistencyDistances.getData().end(), consistencyDistances.getData().begin(), 
			[numSeqs](float x)->float {
				return x / (numSeqs * (numSeqs - 1));
		});
	} else if (config->algorithm.consistency.normalization == SelectivityNormalization::RankedRowStochastic) {
		// rank and normalize distances for ranked 
		for (int i = 0; i < distances.size(); ++i) {
			consistencyDistances[i][i] = std::numeric_limits<float>::max();
		}
		
		for (int i = 0; i < consistencyDistances.size(); ++i) {
			auto inBegin = consistencyDistances.getData().begin() + i * distances.size();
			auto inEnd = inBegin + consistencyDistances.size();
			auto outBegin = consistencyDistances.getData().begin() + i * distances.size();
			rank_range(inBegin, inEnd, outBegin, std::greater<float>());
		}

		std::transform(consistencyDistances.getData().begin(), consistencyDistances.getData().end(), consistencyDistances.getData().begin(), 
			[numSeqs](float x)->float {
				return x / numSeqs;
		});
	
	}
		
	for (float& w : weights) { w = std::max(w, config->algorithm.consistency.saturation); }
	consistencyStage->operator()(weights.data(), *set, consistencyDistances, sparseMatrices);
	
	//compute the final multiple sequence alignment
	TIMER_START(timer);
	weights = tree->getWeights();
	for (float& w : weights) { w = std::max(w, config->algorithm.finalSaturation); }

	auto model = posteriorStage->getModel();
	if (config->hardware.refNumThreads > 0) {
		model->setNumThreads(config->hardware.refNumThreads);
	} else {
		model->setNumThreads(std::max(std::min(config->hardware.numThreads / 2, 8), 1));
	}
	
	omp_set_num_threads(model->getNumThreads());
	auto alignment = constructionStage->operator()(weights.data(), consistencyDistances, tree.get(), *sequences, sparseMatrices, *model);		
	alignment = refinementStage->operator()(*tree, weights.data(), consistencyDistances, sparseMatrices, *model, std::move(alignment));
	TIMER_STOP(timer);
	STATS_WRITE("time.4-final alignment", TIMER_SECONDS(timer));

	// build annotation
	if (config->io.enableAnnotation) {
		WriteAnnotation(alignment.get(), sparseMatrices);
	}

	// delete sparse matrices
	LOG_NORMAL << "Deleting matrices...";
	TIMER_START(timer);
	for (int a = 0; a < numSeqs-1; a++) {
		for (int b = a+1; b < numSeqs; b++) {
			delete sparseMatrices[a][b];
			delete sparseMatrices[b][a];
		}
	}
	TIMER_STOP(timer);
	STATS_WRITE("time.5-delete", TIMER_SECONDS(timer));

	LOG_NORMAL << "ok" << endl;

	TIMER_STOP(timer);
	STATS_WRITE("time.stages-1 to 5", TIMER_SECONDS(timer));

	STATS_WRITE("memory.peak allocated MB", MemoryTools::processPeakVirtual() / (1 << 20));

	computeDatasetStatistics(*sequences, tree->getWeights().data());

	this->joinStats(*tree);
	this->joinStats(*posteriorStage);
	this->joinStats(*consistencyStage);
	this->joinStats(*constructionStage);
	this->joinStats(*refinementStage);
	size_t hash = alignment->calculateHash();
	STATS_WRITE("zhash", hash);
	LOG_DEBUG << "Hash = " << hash << endl;
	
	return alignment;
}



void quickprobs::KernelMSA::iterationDone(const MultiSequence& alignment, int iteration)
{
	if ((config->algorithm.refinement.autosave < std::numeric_limits<int>::max()) 
		&&  (iteration % config->algorithm.refinement.autosave == 0)) {
		string filename = config->io.output + "_r" +std::to_string(iteration);
		ofstream file(filename);
		alignment.WriteMFA(file);
	}
}


void quickprobs::KernelMSA::degenerateDistances(Array<float> &distances) 
{
	float step = 1.0f / (distances.size() * distances.size() / 2);
	float d = step;

	std::vector<int> indices(distances.size());
	std::iota(indices.begin(), indices.end(), 0);
	std::mt19937 gen;
	std::shuffle(indices.begin(), indices.end(), gen);

	std::fill(distances.getData().begin(), distances.getData().end(), 1.0f);

	for (int q = 0; q < indices.size(); ++q) {
		int i = indices[q];
		for (int r = 0; r < q; ++r) {
			int j = indices[r];
			distances[i][j] = distances[j][i] = d;
			d += step;
		}
	}

}

void KernelMSA::printWelcome() {
	 LOG_NORMAL
		<< "*************************************************************************************" << endl
		<< "\t QuickProbs " << QUICKPROBS_VERSION << " (" << __DATE__ << ", " << __TIME__ << ")" << endl 
		<< "\t QuickProbs is a fast and accurate algorithm for multiple sequence alignment" << endl
		<< "\t suited for GPUs." << endl // It uses novel column-based refinement and selective consistency." << endl
		<< "\t Authors: Adam Gudys (adam.gudys@polsl.pl) and Sebastian Deorowicz." << endl
		<< "*************************************************************************************" << endl << endl;
}

void quickprobs::KernelMSA::buildDistancesHistogram(const Array<float>& distances)
{
	int numSeqs = distances.size();
	// build histogram
	if (config->io.enableVerbose) {
		std::vector<float> borders(10);
		std::vector<int> histo (borders.size());
		
		float step = 1.0f / borders.size();
		borders[0] = step;
		for (int i = 1; i < histo.size(); ++i) {
			borders[i] = borders[i - 1] + step;
		}
		
		for (int y = 0; y < numSeqs; ++y) {
			for (int x = y + 1; x < numSeqs; ++x) {
				float d = distances[y][x];
				for (int i = 0; i < borders.size(); ++i) {
					if (d <= borders[i]) {
						++histo[i];
						break;
					}
				}
			}
		}

		for (int i = 0; i < histo.size(); ++i) {
			STATS_WRITE("histo.distances_" + to_string(borders[i]), histo[i]);
		}
	}
}