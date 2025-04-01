#ifndef __THREAD_FIXED_POINT_H
#define __THREAD_FIXED_POINT_H

/* Definitions for fixed-point arithmetic used in thread scheduling. */

typedef int fixed_t;                    /* Define fixed-point type as integer. */

#define FP_SHIFT_AMOUNT 16             /* Number of bits for fractional part. */

/* Convert integer A to fixed-point representation. */
#define FP_CONST(A) ((fixed_t)((A) << FP_SHIFT_AMOUNT))

/* Add two fixed-point values A and B. */
#define FP_ADD(A, B) ((A) + (B))

/* Add fixed-point value A and integer B. */
#define FP_ADD_MIX(A, B) ((A) + ((B) << FP_SHIFT_AMOUNT))

/* Subtract fixed-point value B from A. */
#define FP_SUB(A, B) ((A) - (B))

/* Subtract integer B from fixed-point value A. */
#define FP_SUB_MIX(A, B) ((A) - ((B) << FP_SHIFT_AMOUNT))

/* Multiply fixed-point value A by integer B. */
#define FP_MULT_MIX(A, B) ((A) * (B))

/* Divide fixed-point value A by integer B. */
#define FP_DIV_MIX(A, B) ((A) / (B))

/* Multiply two fixed-point values A and B. */
#define FP_MULT(A, B) ((fixed_t)(((int64_t)(A)) * (B) >> FP_SHIFT_AMOUNT))

/* Divide fixed-point value A by fixed-point value B. */
#define FP_DIV(A, B) ((fixed_t)((((int64_t)(A)) << FP_SHIFT_AMOUNT) / (B)))

/* Get integer part of fixed-point value A. */
#define FP_INT_PART(A) ((A) >> FP_SHIFT_AMOUNT)

/* Round fixed-point value A to nearest integer. */
#define FP_ROUND(A) ((A) >= 0 ? (((A) + (1 << (FP_SHIFT_AMOUNT - 1))) >> FP_SHIFT_AMOUNT) \
                              : (((A) - (1 << (FP_SHIFT_AMOUNT - 1))) >> FP_SHIFT_AMOUNT))

#endif /* __THREAD_FIXED_POINT_H */
