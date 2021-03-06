#define WORDS_BIG_ENDIAN 1
#define BYTES_BIG_ENDIAN 1

#define UNITS_PER_WORD 4
#define MIN_UNITS_PER_WORD 4

#define MIN_UNITS_PER_WORD 4
#define LONG_TYPE_SIZE 32

#define LONG_LONG_TYPE_SIZE 64

#define BITS_PER_UNIT 8
#define ROUND_TOWARDS_ZERO 0
#ifndef LARGEST_EXPONENT_IS_NORMAL
#define LARGEST_EXPONENT_IS_NORMAL(SIZE) 0
#define BITS_PER_WORD (BITS_PER_UNIT * UNITS_PER_WORD)

#if  (defined _ABIN32 && _MIPS_SIM == _ABIN32) \
  || (defined _ABI64 && _MIPS_SIM == _ABI64)
#  define LIBGCC2_LONG_DOUBLE_TYPE_SIZE 128
# else
#  define LIBGCC2_LONG_DOUBLE_TYPE_SIZE 64
# endif

/*typedef unsigned int size_t;*/
#endif
