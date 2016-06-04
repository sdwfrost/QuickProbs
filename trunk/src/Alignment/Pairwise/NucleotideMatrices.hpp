#include "NucleotideMatrices.h"

template <class T>
const T NaiveNucleotide<T>::SCORES[] = {
	1, 0 ,0 ,0, -1,
	0, 1, 0, 0, -1,
	0, 0, 1, 0, -1,
	0, 0, 0, 1, -1,
	-1, -1, -1, -1, -1
};

template <class T>
const T Hoxd55<T>::SCORES[] = {
	91,	-90, -25, -100, -30,
	-90, 100, -100,	-25, -30,
	-25, -100, 100,	-90, -30,
	-100, -25, -90, 91, -30,
	-30, -30, -30, -30, -30
};

template <class T>
const T Hoxd70<T>::SCORES[] = {
	91,	-114, -31, -123, -30,
	-114, 100, -125, -31, -30,
	-31, -125, 100,	-114, -30,
	-123, -31, -114, 91 -30,
	-30, -30, -30, -30, -30
};