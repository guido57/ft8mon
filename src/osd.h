#ifndef OSD_H
#define OSD_H

int osd_decode(const float codeword[174], int depth, int out[91], int *out_depth);
double osd_score(int xplain[91], const float ll174[174]);
void ldpc_encode(int plain[91], int codeword[174]);

#endif // OSD_H