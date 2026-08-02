#ifndef LIBLDPC_H
#define LIBLDPC_H

typedef struct {
    float m[83][174];
    float e[83][174];
    float codeword[174];
    int best_cw[174];
    int cw[174];
    int plain[174];
    int a174[174];
    int best_score;
} ldpc_workspace_t;


    
// Tell C++ that these functions live in C files
#ifdef __cplusplus
extern "C" {
#endif
    void ft8_crc(int msg1[], int msglen, int out[14]);
    void ldpc_decode(float *llr, int max_iter, int *bits, int *nerrors, ldpc_workspace_t *ws);
    

#ifdef __cplusplus
}
#endif


#endif