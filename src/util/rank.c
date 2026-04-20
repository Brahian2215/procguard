#include "rank.h"

int pg_rank_cmp_cpu_desc(const void *a, const void *b)
{
    float fa = ((const ranked_t *)a)->cpu;
    float fb = ((const ranked_t *)b)->cpu;
    return (fb > fa) - (fb < fa); /* descendente, NaN-free por construcción */
}
