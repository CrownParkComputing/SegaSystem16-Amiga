#ifndef AY8910_H
#define AY8910_H
extern int ay8910_index_ym;
void AY8910Write(int chip,int addr,int v);
int  AY8910Read(int chip);
void AY8910Reset(int chip);
void AY8910_set_clock(int chip,int clock);
#endif
