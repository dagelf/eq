/* Raw stereo test tones and independent Fourier amplitude measurement. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define RATE 48000
int main(int argc,char **argv) {
    if (argc==3 && (!strcmp(argv[1],"generate") || !strcmp(argv[1],"noise"))) {
        FILE *f=fopen(argv[2],"wb"); if (!f) return 1;
        for (int i=0;i<RATE*3;i++) {
            float v[2]={.02*sin(2*M_PI*1000*i/RATE),.02*sin(2*M_PI*4000*i/RATE)};
            if (!strcmp(argv[1],"noise")) v[0]=v[1]=.04*((double)rand()/RAND_MAX-.5);
            if (fwrite(v,sizeof(v),1,f)!=1) return 1;
        }
        return fclose(f)!=0;
    }
    if (argc!=5 || strcmp(argv[1],"analyze")) return 2;
    FILE *f=fopen(argv[2],"rb"); if (!f) return 1;
    /* Find the start of the tone, skip settling, then measure exactly one second. */
    float v[2];
    while (fread(v,sizeof(v),1,f)==1 && fabs(v[0])<.00001) {}
    if (fseek(f,RATE/2*sizeof(v),SEEK_CUR)) return 1;
    double re[2]={0},im[2]={0},cross_re[2]={0},cross_im[2]={0};
    for (int i=0;i<RATE;i++) {
        if (fread(v,sizeof(v),1,f)!=1) { fputs("Insufficient recorded audio\n",stderr); return 1; }
        for (int c=0;c<2;c++) { double phase=2*M_PI*(c?4000:1000)*i/RATE,other=2*M_PI*(c?1000:4000)*i/RATE;
            re[c]+=v[c]*cos(phase); im[c]+=v[c]*sin(phase); cross_re[c]+=v[c]*cos(other); cross_im[c]+=v[c]*sin(other); }
    }
    fclose(f);
    for (int c=0;c<2;c++) {
        double gain=20*log10(2*hypot(re[c],im[c])/RATE/.02),cross=20*log10(2*hypot(cross_re[c],cross_im[c])/RATE/.02);
        printf("%s channel %c: %.3f dB, opposite-channel tone %.1f dB\n",argv[2],c?'R':'L',gain,cross);
        if (!isfinite(gain) || fabs(gain-atof(argv[3+c]))>.15 || cross>-65) return 1;
    }
    return 0;
}
