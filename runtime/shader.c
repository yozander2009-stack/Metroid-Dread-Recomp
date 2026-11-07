/* SwitchRecomp - Copyright 2026 MZ Labs
   Licensed under the Apache License 2.0. See LICENSE.txt and NOTICE.txt;
   any copy or derived work must keep the credit to SwitchRecomp by MZ Labs. */
#include <ctype.h>
/* GM20B SASS -> GLSL 4.30. Instruction by instruction, the driver compiles the result and we cache it. */
#include "shader.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>

int g_glsl_version=440;
int g_vs_layer_ext=1;   /* 1 ARB_shader_viewport_layer_array, 2 AMD_vertex_shader_layer, 3 NV_viewport_array2, 0 none */
int g_glsl_es=0;
int g_rs_binding=83;   /* binding of the res-scale UBO, set by gpu_gl */   /* GLSL ES 3.20 (Android) */
#define B(v,lo,n) ((u32)(((v)>>(lo))&((1ull<<(n))-1)))

/* ============================================================ text */
typedef struct { char* p; size_t n, cap; } Sb;

static void sb_put(Sb* s, const char* t){ size_t l=strlen(t); if(s->n+l+1>s->cap){ s->cap=(s->cap+l+1)*2; s->p=realloc(s->p,s->cap); } memcpy(s->p+s->n,t,l+1); s->n+=l; }

static void sb_f(Sb* s, const char* f, ...){ char tmp[2048]; va_list ap; va_start(ap,f); vsnprintf(tmp,sizeof tmp,f,ap); va_end(ap); sb_put(s,tmp); }
/* scratch string ring for building expressions */
static __thread char ring[64][384]; static __thread int ringi;

static char* T(const char* f, ...){ char* r=ring[ringi++&63]; va_list ap; va_start(ap,f); vsnprintf(r,384,f,ap); va_end(ap); return r; }

/* ============================================================ opcode table */
enum {
  O_NONE=0, O_FADD_R,O_FADD_C,O_FADD_I,O_FADD32I, O_FMUL_R,O_FMUL_C,O_FMUL_I,O_FMUL32I, O_FFMA_R,O_FFMA_RC,O_FFMA_CR,O_FFMA_I,O_FFMA32I,
  O_FMNMX_R,O_FMNMX_C,O_FMNMX_I, O_FSETP_R,O_FSETP_C,O_FSETP_I, O_FSET_R,O_FSET_C,O_FSET_I, O_FCMP_R,O_FCMP_RC,O_FCMP_CR,O_FCMP_I,
  O_MUFU, O_RRO_R,O_RRO_C,O_RRO_I, O_F2F_R,O_F2F_C,O_F2F_I, O_F2I_R,O_F2I_C,O_F2I_I, O_I2F_R,O_I2F_C,O_I2F_I, O_I2I_R,O_I2I_C,O_I2I_I,
  O_MOV_R,O_MOV_C,O_MOV_I,O_MOV32I, O_IADD_R,O_IADD_C,O_IADD_I,O_IADD32I, O_IADD3_R,O_IADD3_C,O_IADD3_I, O_ISCADD_R,O_ISCADD_C,O_ISCADD_I,O_ISCADD32I,
  O_IMNMX_R,O_IMNMX_C,O_IMNMX_I, O_ISETP_R,O_ISETP_C,O_ISETP_I, O_ISET_R,O_ISET_C,O_ISET_I, O_ICMP_R,O_ICMP_RC,O_ICMP_CR,O_ICMP_I,
  O_LOP_R,O_LOP_C,O_LOP_I,O_LOP32I, O_LOP3_R,O_LOP3_C,O_LOP3_I, O_SHL_R,O_SHL_C,O_SHL_I, O_SHR_R,O_SHR_C,O_SHR_I,
  O_SHF_LR,O_SHF_LI,O_SHF_RR,O_SHF_RI, O_BFE_R,O_BFE_C,O_BFE_I, O_BFI_R,O_BFI_RC,O_BFI_CR,O_BFI_I, O_SEL_R,O_SEL_C,O_SEL_I,
  O_POPC_R,O_POPC_C,O_POPC_I, O_FLO_R,O_FLO_C,O_FLO_I, O_PRMT_R,O_PRMT_RC,O_PRMT_CR,O_PRMT_I, O_XMAD_R,O_XMAD_RC,O_XMAD_CR,O_XMAD_I,
  O_IMUL_R,O_IMUL_C,O_IMUL_I,O_IMUL32I, O_IMAD_R,O_IMAD_RC,O_IMAD_CR,O_IMAD_I,O_IMAD32I, O_LEA_HR,O_LEA_HC,O_LEA_LR,O_LEA_LC,O_LEA_LI,
  O_PSETP,O_PSET, O_P2R_R,O_P2R_C,O_P2R_I, O_R2P_R,O_R2P_C,O_R2P_I, O_CSET,O_CSETP,
  O_S2R, O_CS2R, O_IPA, O_ALD, O_AST, O_OUT_R,O_OUT_C,O_OUT_I, O_LDC, O_LDG, O_STG, O_LDS, O_STS, O_LDL, O_STL, O_LD, O_ST,
  O_ATOM, O_ATOMS, O_RED, O_ATOM_CAS, O_ATOMS_CAS,
  O_TEX, O_TEX_B, O_TEXS, O_TLD, O_TLDS, O_TLD4, O_TLD4S, O_TXQ, O_TXQ_B, O_TMML, O_TXD,
  O_SULD, O_SUST, O_SURED,
  O_BRA, O_BRX, O_SSY, O_SYNC, O_PBK, O_BRK, O_PCNT, O_CONT, O_EXIT, O_KIL, O_JMP, O_CAL, O_RET, O_PRET, O_PEXIT,
  O_NOP, O_BAR, O_MEMBAR, O_DEPBAR, O_VOTE, O_VOTE_VTG, O_SHFL, O_FSWZADD, O_FCHK_R,O_FCHK_C,O_FCHK_I,
  O_HADD2_R,O_HADD2_C,O_HADD2_I,O_HADD2_32I, O_HMUL2_R,O_HMUL2_C,O_HMUL2_I,O_HMUL2_32I, O_HFMA2_R,O_HFMA2_RC,O_HFMA2_CR,O_HFMA2_I,O_HFMA2_32I,
  O_HSETP2_R,O_HSETP2_C,O_HSETP2_I, O_HSET2_R,O_HSET2_C,O_HSET2_I,
  O_DADD_R,O_DADD_C,O_DADD_I, O_DMUL_R,O_DMUL_C,O_DMUL_I, O_DFMA_R,O_DFMA_RC,O_DFMA_CR,O_DFMA_I, O_DSETP_R,O_DSETP_C,O_DSETP_I, O_DMNMX_R,O_DMNMX_C,O_DMNMX_I,
  O_VMNMX, O_VMAD, O_VSETP, O_VADD, O_IDE, O_CCTL, O_CCTLL, O_BPT, O_ISBERD, O_AL2P, O_PIXLD, O_TXA, O_B2R, O_R2B, O_LEPC, O_SETCRSPTR, O_GETCRSPTR,
  O__COUNT };
typedef struct { const char* pat; int id; const char* name; u64 mask, val; int bits; } Op;
static Op ops[]={
 {"1110111110100---",O_AL2P,"AL2P"},{"1110111111011---",O_ALD,"ALD"},{"1110111111110---",O_AST,"AST"},
 {"11101110111-----",O_ATOM_CAS,"ATOM.CAS"},{"11101101--------",O_ATOM,"ATOM"},{"11101110--------",O_ATOMS_CAS,"ATOMS.CAS"},{"11101100--------",O_ATOMS,"ATOMS"},
 {"1111000010111---",O_B2R,"B2R"},{"1111000010101---",O_BAR,"BAR"},
 {"0101110000000---",O_BFE_R,"BFE"},{"0100110000000---",O_BFE_C,"BFE"},{"0011100-00000---",O_BFE_I,"BFE"},
 {"0101101111110---",O_BFI_R,"BFI"},{"0101001111110---",O_BFI_RC,"BFI"},{"0100101111110---",O_BFI_CR,"BFI"},{"0011011-11110---",O_BFI_I,"BFI"},
 {"111000111010----",O_BPT,"BPT"},{"111000100100----",O_BRA,"BRA"},{"111000110100----",O_BRK,"BRK"},{"111000100101----",O_BRX,"BRX"},
 {"111000100110----",O_CAL,"CAL"},{"11101111011-----",O_CCTL,"CCTL"},{"11101111100-----",O_CCTLL,"CCTLL"},{"111000110101----",O_CONT,"CONT"},
 {"0101000011001---",O_CS2R,"CS2R"},{"0101000010011---",O_CSET,"CSET"},{"0101000010100---",O_CSETP,"CSETP"},
 {"0101110001110---",O_DADD_R,"DADD"},{"0100110001110---",O_DADD_C,"DADD"},{"0011100-01110---",O_DADD_I,"DADD"},
 {"1111000011110---",O_DEPBAR,"DEPBAR"},
 {"010110110111----",O_DFMA_R,"DFMA"},{"010100110111----",O_DFMA_RC,"DFMA"},{"010010110111----",O_DFMA_CR,"DFMA"},{"0011011-0111----",O_DFMA_I,"DFMA"},
 {"0101110001010---",O_DMNMX_R,"DMNMX"},{"0100110001010---",O_DMNMX_C,"DMNMX"},{"0011100-01010---",O_DMNMX_I,"DMNMX"},
 {"0101110010000---",O_DMUL_R,"DMUL"},{"0100110010000---",O_DMUL_C,"DMUL"},{"0011100-10000---",O_DMUL_I,"DMUL"},
 {"010110111000----",O_DSETP_R,"DSETP"},{"010010111000----",O_DSETP_C,"DSETP"},{"0011011-1000----",O_DSETP_I,"DSETP"},
 {"111000110000----",O_EXIT,"EXIT"},
 {"0101110010101---",O_F2F_R,"F2F"},{"0100110010101---",O_F2F_C,"F2F"},{"0011100-10101---",O_F2F_I,"F2F"},
 {"0101110010110---",O_F2I_R,"F2I"},{"0100110010110---",O_F2I_C,"F2I"},{"0011100-10110---",O_F2I_I,"F2I"},
 {"0101110001011---",O_FADD_R,"FADD"},{"0100110001011---",O_FADD_C,"FADD"},{"0011100-01011---",O_FADD_I,"FADD"},{"000010----------",O_FADD32I,"FADD32I"},
 {"0101110010001---",O_FCHK_R,"FCHK"},{"0100110010001---",O_FCHK_C,"FCHK"},{"0011100-10001---",O_FCHK_I,"FCHK"},
 {"010110111010----",O_FCMP_R,"FCMP"},{"010100111010----",O_FCMP_RC,"FCMP"},{"010010111010----",O_FCMP_CR,"FCMP"},{"0011011-1010----",O_FCMP_I,"FCMP"},
 {"010110011-------",O_FFMA_R,"FFMA"},{"010100011-------",O_FFMA_RC,"FFMA"},{"010010011-------",O_FFMA_CR,"FFMA"},{"0011001-1-------",O_FFMA_I,"FFMA"},{"000011----------",O_FFMA32I,"FFMA32I"},
 {"0101110000110---",O_FLO_R,"FLO"},{"0100110000110---",O_FLO_C,"FLO"},{"0011100-00110---",O_FLO_I,"FLO"},
 {"0101110001100---",O_FMNMX_R,"FMNMX"},{"0100110001100---",O_FMNMX_C,"FMNMX"},{"0011100-01100---",O_FMNMX_I,"FMNMX"},
 {"0101110001101---",O_FMUL_R,"FMUL"},{"0100110001101---",O_FMUL_C,"FMUL"},{"0011100-01101---",O_FMUL_I,"FMUL"},{"00011110--------",O_FMUL32I,"FMUL32I"},
 {"01011000--------",O_FSET_R,"FSET"},{"01001000--------",O_FSET_C,"FSET"},{"0011000---------",O_FSET_I,"FSET"},
 {"010110111011----",O_FSETP_R,"FSETP"},{"010010111011----",O_FSETP_C,"FSETP"},{"0011011-1011----",O_FSETP_I,"FSETP"},
 {"0101000011111---",O_FSWZADD,"FSWZADD"},{"111000101100----",O_GETCRSPTR,"GETCRSPTR"},
 {"0101110100010---",O_HADD2_R,"HADD2"},{"0111101-1-------",O_HADD2_C,"HADD2"},{"0111101-0-------",O_HADD2_I,"HADD2"},{"0010110---------",O_HADD2_32I,"HADD2_32I"},
 {"0101110100000---",O_HFMA2_R,"HFMA2"},{"01100---1-------",O_HFMA2_RC,"HFMA2"},{"01110---1-------",O_HFMA2_CR,"HFMA2"},{"01110---0-------",O_HFMA2_I,"HFMA2"},{"0010100---------",O_HFMA2_32I,"HFMA2_32I"},
 {"0101110100001---",O_HMUL2_R,"HMUL2"},{"0111100-1-------",O_HMUL2_C,"HMUL2"},{"0111100-0-------",O_HMUL2_I,"HMUL2"},{"0010101---------",O_HMUL2_32I,"HMUL2_32I"},
 {"0101110100011---",O_HSET2_R,"HSET2"},{"0111110-1-------",O_HSET2_C,"HSET2"},{"0111110-0-------",O_HSET2_I,"HSET2"},
 {"0101110100100---",O_HSETP2_R,"HSETP2"},{"0111111-1-------",O_HSETP2_C,"HSETP2"},{"0111111-0-------",O_HSETP2_I,"HSETP2"},
 {"0101110010111---",O_I2F_R,"I2F"},{"0100110010111---",O_I2F_C,"I2F"},{"0011100-10111---",O_I2F_I,"I2F"},
 {"0101110011100---",O_I2I_R,"I2I"},{"0100110011100---",O_I2I_C,"I2I"},{"0011100-11100---",O_I2I_I,"I2I"},
 {"0101110000010---",O_IADD_R,"IADD"},{"0100110000010---",O_IADD_C,"IADD"},{"0011100-00010---",O_IADD_I,"IADD"},
 {"010111001100----",O_IADD3_R,"IADD3"},{"010011001100----",O_IADD3_C,"IADD3"},{"0011100-1100----",O_IADD3_I,"IADD3"},{"0001110---------",O_IADD32I,"IADD32I"},
 {"010110110100----",O_ICMP_R,"ICMP"},{"010100110100----",O_ICMP_RC,"ICMP"},{"010010110100----",O_ICMP_CR,"ICMP"},{"0011011-0100----",O_ICMP_I,"ICMP"},
 {"111000111001----",O_IDE,"IDE"},
 {"010110100-------",O_IMAD_R,"IMAD"},{"010100100-------",O_IMAD_RC,"IMAD"},{"010010100-------",O_IMAD_CR,"IMAD"},{"0011010-0-------",O_IMAD_I,"IMAD"},{"100000----------",O_IMAD32I,"IMAD32I"},
 {"0101110000100---",O_IMNMX_R,"IMNMX"},{"0100110000100---",O_IMNMX_C,"IMNMX"},{"0011100-00100---",O_IMNMX_I,"IMNMX"},
 {"0101110000111---",O_IMUL_R,"IMUL"},{"0100110000111---",O_IMUL_C,"IMUL"},{"0011100-00111---",O_IMUL_I,"IMUL"},{"00011111--------",O_IMUL32I,"IMUL32I"},
 {"11100000--------",O_IPA,"IPA"},{"1110111111010---",O_ISBERD,"ISBERD"},
 {"0101110000011---",O_ISCADD_R,"ISCADD"},{"0100110000011---",O_ISCADD_C,"ISCADD"},{"0011100-00011---",O_ISCADD_I,"ISCADD"},{"000101----------",O_ISCADD32I,"ISCADD32I"},
 {"010110110101----",O_ISET_R,"ISET"},{"010010110101----",O_ISET_C,"ISET"},{"0011011-0101----",O_ISET_I,"ISET"},
 {"010110110110----",O_ISETP_R,"ISETP"},{"010010110110----",O_ISETP_C,"ISETP"},{"0011011-0110----",O_ISETP_I,"ISETP"},
 {"111000100001----",O_JMP,"JMP"},{"111000110011----",O_KIL,"KIL"},
 {"100-------------",O_LD,"LD"},{"1110111110010---",O_LDC,"LDC"},{"1110111011010---",O_LDG,"LDG"},{"1110111101000---",O_LDL,"LDL"},{"1110111101001---",O_LDS,"LDS"},
 {"0101101111011---",O_LEA_HR,"LEA"},{"000110----------",O_LEA_HC,"LEA"},{"0101101111010---",O_LEA_LR,"LEA"},{"010010111101----",O_LEA_LC,"LEA"},{"0011011-11010---",O_LEA_LI,"LEA"},
 {"0101000011010---",O_LEPC,"LEPC"},
 {"0101110001000---",O_LOP_R,"LOP"},{"0100110001000---",O_LOP_C,"LOP"},{"0011100-01000---",O_LOP_I,"LOP"},
 {"0101101111100---",O_LOP3_R,"LOP3"},{"0000001---------",O_LOP3_C,"LOP3"},{"001111----------",O_LOP3_I,"LOP3"},{"000001----------",O_LOP32I,"LOP32I"},
 {"1110111110011---",O_MEMBAR,"MEMBAR"},
 {"0101110010011---",O_MOV_R,"MOV"},{"0100110010011---",O_MOV_C,"MOV"},{"0011100-10011---",O_MOV_I,"MOV"},{"000000010000----",O_MOV32I,"MOV32I"},
 {"0101000010000---",O_MUFU,"MUFU"},{"0101000010110---",O_NOP,"NOP"},
 {"1111101111100---",O_OUT_R,"OUT"},{"1110101111100---",O_OUT_C,"OUT"},{"1111011011100---",O_OUT_I,"OUT"},
 {"0101110011101---",O_P2R_R,"P2R"},{"0100110011101---",O_P2R_C,"P2R"},{"0011100011101---",O_P2R_I,"P2R"},
 {"111000101010----",O_PBK,"PBK"},{"111000101011----",O_PCNT,"PCNT"},{"111000100011----",O_PEXIT,"PEXIT"},{"1110111111101---",O_PIXLD,"PIXLD"},
 {"0101110000001---",O_POPC_R,"POPC"},{"0100110000001---",O_POPC_C,"POPC"},{"0011100-00001---",O_POPC_I,"POPC"},{"111000100111----",O_PRET,"PRET"},
 {"010110111100----",O_PRMT_R,"PRMT"},{"010100111100----",O_PRMT_RC,"PRMT"},{"010010111100----",O_PRMT_CR,"PRMT"},{"0011011-1100----",O_PRMT_I,"PRMT"},
 {"0101000010001---",O_PSET,"PSET"},{"0101000010010---",O_PSETP,"PSETP"},{"1111000011000---",O_R2B,"R2B"},
 {"0101110011110---",O_R2P_R,"R2P"},{"0100110011110---",O_R2P_C,"R2P"},{"0011100-11110---",O_R2P_I,"R2P"},
 {"1110101111111---",O_RED,"RED"},{"111000110010----",O_RET,"RET"},
 {"0101110010010---",O_RRO_R,"RRO"},{"0100110010010---",O_RRO_C,"RRO"},{"0011100-10010---",O_RRO_I,"RRO"},
 {"1111000011001---",O_S2R,"S2R"},
 {"0101110010100---",O_SEL_R,"SEL"},{"0100110010100---",O_SEL_C,"SEL"},{"0011100-10100---",O_SEL_I,"SEL"},
 {"111000101110----",O_SETCRSPTR,"SETCRSPTR"},
 {"0101101111111---",O_SHF_LR,"SHF.L"},{"0011011-11111---",O_SHF_LI,"SHF.L"},{"0101110011111---",O_SHF_RR,"SHF.R"},{"0011100-11111---",O_SHF_RI,"SHF.R"},
 {"1110111100010---",O_SHFL,"SHFL"},
 {"0101110001001---",O_SHL_R,"SHL"},{"0100110001001---",O_SHL_C,"SHL"},{"0011100-01001---",O_SHL_I,"SHL"},
 {"0101110000101---",O_SHR_R,"SHR"},{"0100110000101---",O_SHR_C,"SHR"},{"0011100-00101---",O_SHR_I,"SHR"},
 {"111000101001----",O_SSY,"SSY"},{"101-------------",O_ST,"ST"},{"1110111011011---",O_STG,"STG"},{"1110111101010---",O_STL,"STL"},{"1110111101011---",O_STS,"STS"},
 {"11101011000-----",O_SULD,"SULD"},{"11101011001-----",O_SUST,"SUST"},{"11101011010-----",O_SURED,"SURED"},
 {"1111000011111---",O_SYNC,"SYNC"},
 {"11000-----------",O_TEX,"TEX"},{"1101111010------",O_TEX_B,"TEX.B"},{"1101-00---------",O_TEXS,"TEXS"},
 {"11011100--------",O_TLD,"TLD"},{"110010----------",O_TLD4,"TLD4"},{"11011111-0------",O_TLD4S,"TLD4S"},{"1101-01---------",O_TLDS,"TLDS"},
 {"1101111101011---",O_TMML,"TMML"},{"1101111101000---",O_TXA,"TXA"},{"1101111000------",O_TXD,"TXD"},
 {"1101111101001---",O_TXQ,"TXQ"},{"1101111101010---",O_TXQ_B,"TXQ.B"},
 {"0101000011011---",O_VOTE,"VOTE"},{"0101000011100---",O_VOTE_VTG,"VOTE.VTG"},
 {"0011101---------",O_VMNMX,"VMNMX"},{"01011111--------",O_VMAD,"VMAD"},{"0101000011110---",O_VSETP,"VSETP"},{"001000----------",O_VADD,"VADD"},
 {"010110110-------",O_XMAD_R,"XMAD"},{"010100010-------",O_XMAD_RC,"XMAD"},{"0100111---------",O_XMAD_CR,"XMAD"},{"0011011-00------",O_XMAD_I,"XMAD"},
 {NULL,0,NULL}};
static int ops_ready;

static int opcmp(const void* a, const void* b){ const Op* x=a,*y=b; return y->bits-x->bits; }

static const Op* op_decode[65536];
static const Op* op_nop;
static void ops_init(void){
  int n=0; for(;ops[n].pat;n++){ u64 m=0,v=0; int bits=0; for(int i=0;i<16;i++){ char ch=ops[n].pat[i]; u64 bit=1ull<<(63-i); if(ch=='0'||ch=='1'){ m|=bit; bits++; if(ch=='1') v|=bit; } } ops[n].mask=m; ops[n].val=v; ops[n].bits=bits; }
  qsort(ops,n,sizeof(Op),opcmp);
  /* Every opcode mask uses only the upper 16 bits. Preserve the ordered
     decoder's first match, including equally specific overlapping patterns. */
  for(u32 word=0;word<65536;word++){u64 w=(u64)word<<48;for(int i=0;i<n;i++)if((w&ops[i].mask)==ops[i].val){op_decode[word]=&ops[i];break;}}
  for(int i=0;i<n;i++)if(ops[i].id==O_NOP){op_nop=&ops[i];break;}
  ops_ready=1;
}
/* EXIT.FCSM_TR after VOTE.VTG is a hw cull exit; treat as NOP or the VS never writes position */
static const Op* decode(u64 w){const Op*op=op_decode[w>>48];return op&&op->id==O_EXIT&&(w&31)==0x1c&&op_nop?op_nop:op;}

/* ============================================================ context */
typedef struct {
  const u64* w; u32 nwords; ShaderInfo* si; int stage;
  Sb out; Sb decl;
  u8 used_reg[256]; u8 used_pred[8]; int uses_cc;
  u8 in_used[32], out_used[32];
  int uses_local, uses_shared, uses_img_raw;
  u8 leader[65536/8];
  int max_idx;
  int has_flow;
  int indent;
  u8 gtag[256];        /* reg -> global buffer it holds the base of (index+1) */
  u8 tgt[65536/8];     /* branch targets (BRA/SSY/PBK/PCNT and BRX table) */
  u32* pop_tgt;        /* static stack resolution: target of each SYNC/BRK/CONT (0 = unreachable) */
  int stk_static;      /* 1 if SSY/PBK/PCNT got resolved statically (no stack arrays emitted) */
  int uses_stk[3];     /* which dynamic stacks are used: 0 ssy 1 pbk 2 pcnt */
  u8 wtag[256];        /* 1: reg = IPA(position.w), 2: reg = RCP of that (same block only) */
  int tag_r, tag_v;    /* label set by the current instruction */
  int nest;            /* structured mode: inside an if, so don't label anything (doesn't dominate the rest) */
} Ctx;
#define IDX_OK(i) ((i)>=11 && ((i)-10)%4!=0)

static char* R(Ctx* c, u32 r){ if(r==255) return "0u"; c->used_reg[r]=1; return T("r%u",r); }

static char* RF(Ctx* c, u32 r){ if(r==255) return "0.0"; c->used_reg[r]=1; return T("uintBitsToFloat(r%u)",r); }

static char* P(Ctx* c, u32 p){ if(p==7) return "true"; c->used_pred[p]=1; return T("p%u",p); }

static char* PN(Ctx* c, u32 p, int neg){ char* s=P(c,p); if(neg) return T("(!%s)",s); return s; }

static char* CB(Ctx* c, u32 idx, u32 off){ c->si->cbuf_mask|=1u<<idx; if(idx<18 && off+16>c->si->cbuf_max[idx]) c->si->cbuf_max[idx]=(off|15)+1; return T("floatBitsToUint(cb%u.d[%u].%c)",idx,off>>4,"xyzw"[(off>>2)&3]); }

static char* CBF(Ctx* c, u32 idx, u32 off){ c->si->cbuf_mask|=1u<<idx; if(idx<18 && off+16>c->si->cbuf_max[idx]) c->si->cbuf_max[idx]=(off|15)+1; return T("cb%u.d[%u].%c",idx,off>>4,"xyzw"[(off>>2)&3]); }

static void emitl(Ctx* c, const char* f, ...){ char tmp[4096]; va_list ap; va_start(ap,f); vsnprintf(tmp,sizeof tmp,f,ap); va_end(ap);
  for(int i=0;i<c->indent;i++) sb_put(&c->out,"  "); sb_put(&c->out,tmp); sb_put(&c->out,"\n"); }

static void setr(Ctx* c, u32 r, const char* e){ if(r==255) return;   /* RZ: discard */ c->used_reg[r]=1; emitl(c,"r%u = %s;",r,e); }

static void setrf(Ctx* c, u32 r, const char* e){ setr(c,r,T("floatBitsToUint(%s)",e)); }

static void setp(Ctx* c, u32 p, const char* e){ if(p==7) return; c->used_pred[p]=1; emitl(c,"p%u = %s;",p,e); }
static const char* bop(u32 op, const char* a, const char* b);

/* write the predicate pair for FSETP/ISETP/PSETP/CSETP, skip the temp if only one is used */
static void setp2(Ctx* c, u32 pa, u32 pb, u32 bo, const char* t, const char* pc){
  if(pa!=7 && pb!=7){ emitl(c,"{ bool t_ = %s;",t); c->indent++; setp(c,pa,bop(bo,"t_",pc)); setp(c,pb,bop(bo,"(!t_)",pc)); c->indent--; emitl(c,"}"); }
  else if(pa!=7) setp(c,pa,bop(bo,t,pc));
  else if(pb!=7) setp(c,pb,bop(bo,T("(!%s)",t),pc)); }

/* operand B per encoding */
static u32 cb_idx(u64 w){ return B(w,34,5); }

static u32 cb_off(u64 w){ return B(w,20,14)*4; }

/* is c0[0x110+16k] an NVN ssbo address? returns k+1 or 0 */
static u8 gbuf_tag(Ctx* c, u64 w){
  /* gfx ssbos live at c0[0x110..0x210], compute {addr,size} pairs at c0[0x210..0x410] */
  u32 lo=0x110, hi= c->stage==ST_CS? 0x410 : 0x210;   /* (NVN compute uses 0x310.., deko3d 0x110..) */
  if(cb_idx(w)!=0) return 0; u32 off=cb_off(w); if(off<lo || off>=hi || (off&15)) return 0;
  ShaderInfo* si=c->si; for(int i=0;i<si->ngbuf;i++) if(si->gbuf_cbuf_off[i]==off) return (u8)(i+1);
  if(si->ngbuf>=(c->stage==ST_CS? 16 : 8)) return 0; si->gbuf_cbuf_off[si->ngbuf++]=off; return (u8)si->ngbuf; }

static char* imm20f(u64 w){ u32 v=(B(w,20,19)<<12)|(B(w,56,1)<<31); return T("uintBitsToFloat(0x%Xu)",v); }

static u32 imm20i_v(u64 w){ u32 v=B(w,20,19); if(B(w,56,1)) v|=0xFFF80000u; return v; }

static char* imm20i(u64 w){ return T("0x%Xu",imm20i_v(w)); }

static char* imm32(u64 w){ return T("0x%Xu",B(w,20,32)); }

static char* imm32f(u64 w){ return T("uintBitsToFloat(0x%Xu)",B(w,20,32)); }

/* kind: 0 reg(20) 1 cbuf 2 imm20 3 imm32 4 reg39 */
static char* opB(Ctx* c, u64 w, int kind){ switch(kind){ case 0: return R(c,B(w,20,8)); case 1: return CB(c,cb_idx(w),cb_off(w)); case 2: return imm20i(w); case 3: return imm32(w); default: return R(c,B(w,39,8)); } }

static char* opBf(Ctx* c, u64 w, int kind){ switch(kind){ case 0: return RF(c,B(w,20,8)); case 1: return CBF(c,cb_idx(w),cb_off(w)); case 2: return imm20f(w); case 3: return imm32f(w); default: return RF(c,B(w,39,8)); } }

static int kind_of(int id, int base){ return id-base; }

static char* fneg_abs(const char* e, int neg, int abs){ const char* x=e; if(abs) x=T("abs(%s)",x); if(neg) x=T("(-%s)",x); return (char*)x; }

static char* fsat(const char* e, int sat){ return sat? T("clamp(%s,0.0,1.0)",e) : (char*)e; }

static const char* fcmp_expr(u32 op, const char* a, const char* b){
  switch(op){ case 0: return "false"; case 1: return T("(%s < %s)",a,b); case 2: return T("(%s == %s)",a,b); case 3: return T("(%s <= %s)",a,b);
    case 4: return T("(%s > %s)",a,b); case 5: return T("(%s != %s && !isnan(%s) && !isnan(%s))",a,b,a,b); case 6: return T("(%s >= %s)",a,b);
    case 7: return T("(!isnan(%s) && !isnan(%s))",a,b); case 8: return T("(isnan(%s) || isnan(%s))",a,b);
    case 9: return T("(%s < %s || isnan(%s) || isnan(%s))",a,b,a,b); case 10: return T("(%s == %s || isnan(%s) || isnan(%s))",a,b,a,b);
    case 11: return T("(%s <= %s || isnan(%s) || isnan(%s))",a,b,a,b); case 12: return T("(%s > %s || isnan(%s) || isnan(%s))",a,b,a,b);
    case 13: return T("(%s != %s)",a,b); case 14: return T("(%s >= %s || isnan(%s) || isnan(%s))",a,b,a,b); default: return "true"; } }

static const char* icmp_expr(u32 op, const char* a, const char* b, int sgn){
  const char* A= sgn? T("int(%s)",a) : a; const char* Bb= sgn? T("int(%s)",b) : b;
  switch(op){ case 0: return "false"; case 1: return T("(%s < %s)",A,Bb); case 2: return T("(%s == %s)",A,Bb); case 3: return T("(%s <= %s)",A,Bb);
    case 4: return T("(%s > %s)",A,Bb); case 5: return T("(%s != %s)",A,Bb); case 6: return T("(%s >= %s)",A,Bb); default: return "true"; } }

static const char* bop(u32 op, const char* a, const char* b){
  if(!strcmp(b,"true")){ if(op==0) return a; if(op==1) return "true"; if(op==2) return T("(!%s)",a); }       /* PT */
  else if(!strcmp(b,"(!true)")){ if(op==0) return "false"; if(op==1||op==2) return a; }                   /* !PT */
  switch(op){ case 0: return T("(%s && %s)",a,b); case 1: return T("(%s || %s)",a,b); case 2: return T("(%s != %s)",a,b); } return a; }

/* ============================================================ attributes */
static char* attr_read(Ctx* c, u32 addr, int is_output){
  u32 comp=(addr>>2)&3;
  if(addr>=0x80 && addr<0x280){ u32 i=(addr-0x80)>>4;
    if(is_output){ c->out_used[i]=1; return T("floatBitsToUint(out_attr%u.%c)",i,"xyzw"[comp]); }
    c->in_used[i]=1;
    if(c->stage==ST_VS && (c->si->attr_int_mask>>i)&1) return T("uint(in_attr%u.%c)",i,"xyzw"[comp]);
    return T("floatBitsToUint(in_attr%u.%c)",i,"xyzw"[comp]); }
  if(addr>=0x70 && addr<0x80){
    if(c->stage==ST_FS){ c->si->uses_position_in=1; if(comp==3) return "floatBitsToUint(1.0/gl_FragCoord.w)"; if(comp<2) return T("floatBitsToUint(gl_FragCoord.%c*swr_rs.x)","xy"[comp]); return T("floatBitsToUint(gl_FragCoord.%c)","xyzw"[comp]); }
    if(c->stage==ST_VS) return T("floatBitsToUint(gl_Position.%c)","xyzw"[comp]);
    return T("floatBitsToUint(gl_in[0].gl_Position.%c)","xyzw"[comp]); }
  switch(addr){
    case 0x2FC: return c->stage==ST_VS? "uint(gl_VertexID)" : "0u";
    case 0x2F8: return c->stage==ST_VS? "uint(gl_InstanceID)" : "0u";
    case 0x3FC: c->si->uses_front_face=1; return c->stage==ST_FS? "(gl_FrontFacing? 0xFFFFFFFFu : 0u)" : "0u";
    case 0x60: return c->stage==ST_FS? "uint(gl_PrimitiveID)" : "0u";
    case 0x64: return c->stage==ST_FS? "uint(gl_Layer)" : "0u";
    case 0x68: return "0u";
    case 0x6C: return "floatBitsToUint(1.0)";
    case 0x2E0: case 0x2E4: return c->stage==ST_FS? T("floatBitsToUint(gl_PointCoord.%c)","xy"[(addr>>2)&1]) : "0u";
  }
  if(addr>=0x2C0 && addr<0x2E0) return "0u";
  return "0u";
}

static void attr_write(Ctx* c, u32 addr, const char* val){
  u32 comp=(addr>>2)&3;
  if(addr>=0x80 && addr<0x280){ u32 i=(addr-0x80)>>4; c->out_used[i]=1; emitl(c,"out_attr%u.%c = uintBitsToFloat(%s);",i,"xyzw"[comp],val); return; }
  if(addr>=0x70 && addr<0x80){ emitl(c,"gl_Position.%c = uintBitsToFloat(%s);","xyzw"[comp],val); return; }
  switch(addr){
    case 0x6C: c->si->writes_point_size=1; emitl(c,"gl_PointSize = uintBitsToFloat(%s);",val); return;
    case 0x64: c->si->writes_layer=1; emitl(c,"SWR_SET_LAYER(int(%s));",val); return;
    case 0x68: emitl(c,"/* viewport index */"); return; }
  if(addr>=0x2C0 && addr<0x2E0 && g_glsl_es){ emitl(c,"/* clip %u: %s */",(addr-0x2C0)>>2,val); return; }
  if(addr>=0x2C0 && addr<0x2E0){ emitl(c,"gl_ClipDistance[%u] = uintBitsToFloat(%s);",(addr-0x2C0)>>2,val); c->si->uses_clip_mask|=1<<((addr-0x2C0)>>2); return; }
  emitl(c,"/* escritura a atributo %x ignorada */",addr);
}

/* ============================================================ textures */
static u8 g_bl2; static u32 g_bl2_bank, g_bl2_off;   /* second handle read for split handles, set by track_handle */
static int tex_slot2(Ctx* c, u32 cbuf_off_words, int type, int array, int shadow, int fetch, int bl, u32 bank);

static int tex_slot(Ctx* c, u32 cbuf_off_words, int type, int array, int shadow, int fetch){ return tex_slot2(c,cbuf_off_words,type,array,shadow,fetch,0,0); }

static int tex_slot2(Ctx* c, u32 cbuf_off_words, int type, int array, int shadow, int fetch, int bl, u32 bank){
  ShaderInfo* si=c->si;
  u8 b2= bl? g_bl2 : 0;
  for(int i=0;i<si->ntex;i++){ ShTex* t=&si->tex[i]; if(t->cbuf_off==cbuf_off_words && t->type==type && t->array==array && t->shadow==shadow && t->bindless==bl && (!bl || t->bl_cbuf==bank)
      && t->bl2==b2 && (!b2 || (t->bl2_cbuf==g_bl2_bank && t->bl2_off==g_bl2_off))) { if(!fetch) t->fetch_only=0; return i; } }
  if(si->ntex>=SH_MAXTEX) return 0;
  ShTex* t=&si->tex[si->ntex]; memset(t,0,sizeof *t); t->cbuf_off=cbuf_off_words; t->type=(u8)type; t->array=(u8)array; t->shadow=(u8)shadow; t->fetch_only=(u8)fetch;
  t->bindless=(u8)bl; t->bl_cbuf=(u8)bank; if(bl && bank<18){ si->cbuf_mask|=1u<<bank; if(cbuf_off_words*4+16>si->cbuf_max[bank]) si->cbuf_max[bank]=((cbuf_off_words*4)|15)+1; }
  if(b2){ t->bl2=1; t->bl2_cbuf=(u8)g_bl2_bank; t->bl2_off=g_bl2_off; u32 b=g_bl2_bank;
    if(b<18){ si->cbuf_mask|=1u<<b; if(g_bl2_off*4+16>si->cbuf_max[b]) si->cbuf_max[b]=((g_bl2_off*4)|15)+1; } }
  return si->ntex++; }
/* TEX.B / TXQ.B: handle comes in a register. Find the LDC / MOV from cbuf that produced it and treat it
   as a regular bound texture from that bank. */
static const Op* decode(u64 w);

/* backtrack rb before idx. handles show up as a plain LDC, a MOV from cbuf, reg copies, or OR/ADD of two
   LDCs (separate tex + sampler, Unity does this). returns up to 2 (bank, byte offset) reads */
static int track_src(Ctx* c, u32 idx, u32 rb, u32* bank, u32* off, int* n, int depth){
  if(rb==255 || depth>6) return 0;
  for(u32 k=1;k<=1024 && k<idx;k++){ u32 j=idx-k; if(!IDX_OK(j)) continue; u64 w=c->w[j]; const Op* o=decode(w); if(!o) continue;
    if(B(w,0,8)!=rb) continue;
    const char* nm=o->name;
    if(strstr(nm,"SETP") || !strncmp(nm,"ST",2) || !strncmp(nm,"BR",2) || !strcmp(nm,"EXIT") || !strcmp(nm,"SSY") || !strcmp(nm,"SYNC") || !strcmp(nm,"PBK") || !strcmp(nm,"PCNT") || !strcmp(nm,"CONT")
       || !strcmp(nm,"KIL") || !strcmp(nm,"NOP") || !strcmp(nm,"DEPBAR") || !strcmp(nm,"BAR") || !strcmp(nm,"AST") || !strcmp(nm,"RED") || !strcmp(nm,"MEMBAR") || !strcmp(nm,"CAL") || !strcmp(nm,"RET")) continue;
    if(o->id==O_LDC){ u32 sz=B(w,48,3); if(sz!=4 && sz!=5) return 0; if(*n>=2) return 0;
      /* indexed handle (array of textures): just use element 0 */
      bank[*n]=B(w,36,5); off[*n]=(u32)((i32)(B(w,20,16)<<16)>>16); if(off[*n]&3) return 0; (*n)++; return 1; }
    if(o->id==O_MOV_C){ if(*n>=2) return 0; bank[*n]=cb_idx(w); off[*n]=cb_off(w); (*n)++; return 1; }
    if(o->id==O_MOV_R) return track_src(c,j,B(w,20,8),bank,off,n,depth+1);
    if(o->id==O_LOP_R || o->id==O_LOP3_R || o->id==O_IADD_R){
      u32 ra=B(w,8,8), rbb=B(w,20,8);
      if(ra==255) return track_src(c,j,rbb,bank,off,n,depth+1);
      if(rbb==255) return track_src(c,j,ra,bank,off,n,depth+1);
      return track_src(c,j,ra,bank,off,n,depth+1) && track_src(c,j,rbb,bank,off,n,depth+1); }
    if(o->id==O_LOP32I || o->id==O_LOP_I || o->id==O_IADD32I || o->id==O_IADD_I || o->id==O_LOP_C || o->id==O_IADD_C)
      return track_src(c,j,B(w,8,8),bank,off,n,depth+1);
    return 0; }
  return 0; }

static int track_handle(Ctx* c, u32 idx, u32 rb, u32* bank, u32* off){
  u32 bk[2]={0,0}, of[2]={0,0}; int n=0; g_bl2=0;
  if(!track_src(c,idx,rb,bk,of,&n,0) || !n) return 0;
  *bank=bk[0]; *off=of[0];
  if(n==2){ g_bl2=1; g_bl2_bank=bk[1]; g_bl2_off=of[1]/4; }
  return 1; }

static const char* IW(Ctx* c, int s, const char* smp){
  if(s<0 || s>=32 || !((c->si->tex_int_mask>>s)&1)) return smp;
  return T("uintBitsToFloat(uvec4(%s))",smp); }

static const char* samp_type(int type, int array, int shadow){
  /* 1D textures are created as GL_TEXTURE_2D (tex_target), so sample them as 2D on desktop too */
  static const char* n[4]={"1D","2D","3D","Cube"}; if(type==0) type=1;
  return T("sampler%s%s%s",n[type&3],array?"Array":"",shadow?"Shadow":""); }

/* store masked components of vec4 v into d, d+1, ... */
static void write_masked(Ctx* c, u32 d, u32 mask, const char* v){
  u32 r=d; for(int k=0;k<4;k++) if(mask&(1u<<k)){ if(r!=255) setrf(c,r,T("%s.%c",v,"xyzw"[k])); r= r==255? 255 : r+1; } }

/* TEXS/TLDS/TLD4S write two reg pairs */
static void write_texs(Ctx* c, u64 w, const char* v, int fp16){
  u32 da=B(w,0,8), db=B(w,28,8), sw=B(w,50,3);
  int comps[4]; int n=0;
  if(db==255){ static const int tab[8][2]={{0,-1},{1,-1},{2,-1},{3,-1},{0,1},{0,3},{1,3},{2,3}}; for(int k=0;k<2;k++) if(tab[sw][k]>=0) comps[n++]=tab[sw][k]; }
  else { static const int tab[5][4]={{0,1,2,-1},{0,1,3,-1},{0,2,3,-1},{1,2,3,-1},{0,1,2,3}}; if(sw>4) sw=4; for(int k=0;k<4;k++) if(tab[sw][k]>=0) comps[n++]=tab[sw][k]; }
  if(fp16){
    if(n>0) setr(c,da,n>1? T("packHalf2x16(vec2(%s.%c,%s.%c))",v,"xyzw"[comps[0]],v,"xyzw"[comps[1]]) : T("packHalf2x16(vec2(%s.%c,0.0))",v,"xyzw"[comps[0]]));
    if(n>2) setr(c,db,n>3? T("packHalf2x16(vec2(%s.%c,%s.%c))",v,"xyzw"[comps[2]],v,"xyzw"[comps[3]]) : T("packHalf2x16(vec2(%s.%c,0.0))",v,"xyzw"[comps[2]]));
    return; }
  for(int k=0;k<n;k++){ u32 r= k<2? da+k : db+(k-2); if(k<2 && da==255) continue; if(k>=2 && db==255) continue; setrf(c,r,T("%s.%c",v,"xyzw"[comps[k]])); }
}

/* ============================================================ per-instruction translation */
static u32 bra_target(u64 w, u32 idx){ i32 off=(i32)(B(w,20,24)<<8)>>8; u32 t=(u32)((i32)idx+1+off/8);
  if(t>=10 && (t-10)%4==0) t++;       /* sched control word, skip */
  return t; }

static void unsupported(Ctx* c, const char* name, u64 w){ c->si->unsupported++; if(!c->si->first_unsupported[0]) snprintf(c->si->first_unsupported,64,"%s %016llx",name,(unsigned long long)w); emitl(c,"/* untranslated: %s */",name); }

static void inst(Ctx* c, u32 idx, u64 w, const Op* op);

static void emit_guarded(Ctx* c, u32 idx, u64 w, const Op* op){
  u32 pg=B(w,16,3), pn=B(w,19,1);
  int id=op->id;
  int is_flow= id==O_BRA||id==O_SYNC||id==O_BRK||id==O_EXIT||id==O_KIL||id==O_CONT||id==O_SSY||id==O_PBK||id==O_PCNT;
  /* SSY/PBK/PCNT/PRET/CAL ignore the predicate field (reads as P0) */
  if(id==O_SSY||id==O_PBK||id==O_PCNT||id==O_PRET||id==O_CAL||id==O_PEXIT){ inst(c,idx,w,op); return; }
  if(pg==7 && pn){ emitl(c,"/* never: %s */",op->name); return; }
  if(pg==7 || is_flow==2){ inst(c,idx,w,op); return; }
  emitl(c,"if(%s) {",PN(c,pg,pn)); c->indent++; inst(c,idx,w,op); c->indent--; emitl(c,"}");
}

/* emit + label bookkeeping: any rN write kills its label, only unpredicated instrs set one */
static void emit_tracked(Ctx* c, u32 idx, u64 w, const Op* op){
  size_t s0=c->out.n; c->tag_r=-1; c->tag_v=0;
  emit_guarded(c,idx,w,op);
  for(size_t k=s0;k+1<c->out.n;k++){ const char* p=c->out.p+k;
    if(p[0]=='r' && p[1]>='0' && p[1]<='9' && (k==s0 || !(isalnum((unsigned char)p[-1])||p[-1]=='_'))){
      u32 r=0; const char* q=p+1; while(*q>='0' && *q<='9'){ r=r*10+(u32)(*q-'0'); q++; }
      if(q[0]==' ' && q[1]=='=' && q[2]!='=' && r<256) c->wtag[r]=0; } }
  if(c->tag_r>=0 && c->tag_r<255 && !c->nest && B(w,16,3)==7 && !B(w,19,1)) c->wtag[c->tag_r]=(u8)c->tag_v;
  c->tag_r=-1; }

static void exit_epilogue(Ctx* c){
  if(c->stage==ST_FS){
    u32 target=c->si->sph[18], flags=c->si->sph[19]; u32 reg=0;
    for(int rt=0;rt<8;rt++){ u32 m=(target>>(rt*4))&15; if(!m) continue; c->si->frag_rt_mask|=1u<<rt;
      for(int k=0;k<4;k++){ if(m&(1u<<k)) { if((c->si->rt_int_mask>>rt)&1) emitl(c,"frag_color%d.%c = %s;",rt,"xyzw"[k], ((c->si->rt_sint_mask>>rt)&1)? T("int(%s)",R(c,reg)) : R(c,reg)); else emitl(c,"frag_color%d.%c = %s;",rt,"xyzw"[k],RF(c,reg)); } reg++; } }
    /* the sample mask sits at colors_end and depth at colors_end+1, with or without a sample mask */
    if(flags&2){ c->si->writes_depth=1; emitl(c,"gl_FragDepth = %s;",RF(c,reg+1)); }
  }
  emitl(c,"return;");
}

static void inst(Ctx* c, u32 idx, u64 w, const Op* op){
  u32 d=B(w,0,8), a=B(w,8,8);
  int id=op->id;
  switch(id){
  /* ---------------- float */
  case O_FADD_R: case O_FADD_C: case O_FADD_I: {
    char* A=fneg_abs(RF(c,a),B(w,48,1),B(w,46,1)); char* Bv=fneg_abs(opBf(c,w,id-O_FADD_R),B(w,45,1),B(w,49,1));
    setrf(c,d,fsat(T("(%s + %s)",A,Bv),B(w,50,1))); break; }
  case O_FADD32I: { char* A=fneg_abs(RF(c,a),B(w,56,1),B(w,54,1)); char* Bv=fneg_abs(imm32f(w),B(w,53,1),B(w,57,1)); setrf(c,d,T("(%s + %s)",A,Bv)); break; }
  case O_FMUL_R: case O_FMUL_C: case O_FMUL_I: {
    char* Bv=opBf(c,w,id-O_FMUL_R); if(B(w,48,1)) Bv=T("(-%s)",Bv);
    char* e= B(w,44,2)==2? T("fmz(%s, %s)",RF(c,a),Bv) : T("(%s * %s)",RF(c,a),Bv); u32 sc=B(w,41,3);   /* 0*x=0 only matters for .FMZ */
    static const char* scl[8]={"","*0.5","*0.25","*0.125","*8.0","*4.0","*2.0",""}; if(sc) e=T("(%s%s)",e,scl[sc]);
    setrf(c,d,fsat(e,B(w,50,1))); break; }
  case O_FMUL32I: setrf(c,d,fsat(B(w,53,2)==2? T("fmz(%s, %s)",RF(c,a),imm32f(w)) : T("(%s * %s)",RF(c,a),imm32f(w)),B(w,55,1))); break;
  case O_FFMA_R: case O_FFMA_RC: case O_FFMA_CR: case O_FFMA_I: {
    char *Bv,*Cv;
    if(id==O_FFMA_R){ Bv=RF(c,B(w,20,8)); Cv=RF(c,B(w,39,8)); } else if(id==O_FFMA_RC){ Bv=RF(c,B(w,39,8)); Cv=CBF(c,cb_idx(w),cb_off(w)); }
    else if(id==O_FFMA_CR){ Bv=CBF(c,cb_idx(w),cb_off(w)); Cv=RF(c,B(w,39,8)); } else { Bv=imm20f(w); Cv=RF(c,B(w,39,8)); }
    if(B(w,48,1)) Bv=T("(-%s)",Bv); if(B(w,49,1)) Cv=T("(-%s)",Cv);
    setrf(c,d,fsat(B(w,53,2)==2? T("ffmz(%s, %s, %s)",RF(c,a),Bv,Cv) : T("fma(%s, %s, %s)",RF(c,a),Bv,Cv),B(w,50,1))); break; }
  case O_FFMA32I: { char* A=RF(c,a); if(B(w,56,1)) A=T("(-%s)",A); char* Cv=RF(c,d); if(B(w,57,1)) Cv=T("(-%s)",Cv);
    setrf(c,d,fsat(B(w,53,2)==2? T("ffmz(%s, %s, %s)",A,imm32f(w),Cv) : T("fma(%s, %s, %s)",A,imm32f(w),Cv),B(w,55,1))); break; }
  case O_FMNMX_R: case O_FMNMX_C: case O_FMNMX_I: {
    char* A=fneg_abs(RF(c,a),B(w,48,1),B(w,46,1)); char* Bv=fneg_abs(opBf(c,w,id-O_FMNMX_R),B(w,45,1),B(w,49,1));
    setrf(c,d,T("(%s ? min(%s, %s) : max(%s, %s))",PN(c,B(w,39,3),B(w,42,1)),A,Bv,A,Bv)); break; }
  case O_FSETP_R: case O_FSETP_C: case O_FSETP_I: {
    char* A=fneg_abs(RF(c,a),B(w,43,1),B(w,7,1)); char* Bv=fneg_abs(opBf(c,w,id-O_FSETP_R),B(w,6,1),B(w,44,1));
    const char* t=fcmp_expr(B(w,48,4),A,Bv); const char* pc=PN(c,B(w,39,3),B(w,42,1));
    setp2(c,B(w,3,3),B(w,0,3),B(w,45,2),t,pc); break; }
  case O_FSET_R: case O_FSET_C: case O_FSET_I: {
    char* A=fneg_abs(RF(c,a),B(w,43,1),B(w,54,1)); char* Bv=fneg_abs(opBf(c,w,id-O_FSET_R),B(w,53,1),B(w,44,1));
    const char* t=bop(B(w,45,2),fcmp_expr(B(w,48,4),A,Bv),PN(c,B(w,39,3),B(w,42,1)));
    setr(c,d,T("(%s ? %s : 0u)",t,B(w,52,1)? "floatBitsToUint(1.0)" : "0xFFFFFFFFu")); break; }
  case O_FCMP_R: case O_FCMP_RC: case O_FCMP_CR: case O_FCMP_I: {
    char *Bv,*Cv;
    if(id==O_FCMP_R){ Bv=R(c,B(w,20,8)); Cv=RF(c,B(w,39,8)); } else if(id==O_FCMP_RC){ Bv=R(c,B(w,39,8)); Cv=CBF(c,cb_idx(w),cb_off(w)); }
    else if(id==O_FCMP_CR){ Bv=CB(c,cb_idx(w),cb_off(w)); Cv=RF(c,B(w,39,8)); } else { Bv=T("floatBitsToUint(%s)",imm20f(w)); Cv=RF(c,B(w,39,8)); }
    setr(c,d,T("(%s ? %s : %s)",fcmp_expr(B(w,48,4),Cv,"0.0"),R(c,a),Bv)); break; }
  case O_MUFU: {
    char* A=fneg_abs(RF(c,a),B(w,48,1),B(w,46,1)); const char* e;
    switch(B(w,20,4)){ case 0: e=T("cos(%s)",A); break; case 1: e=T("sin(%s)",A); break; case 2: e=T("exp2(%s)",A); break; case 3: e=T("log2(%s)",A); break;
      case 4: e=T("(1.0/%s)",A); break; case 5: e=T("inversesqrt(%s)",A); break; case 8: e=T("sqrt(%s)",A); break; default: e=A; }
    if(B(w,20,4)==4 && !B(w,48,1) && !B(w,46,1) && !B(w,50,1) && a!=255 && c->wtag[a]==1){ c->tag_r=(int)d; c->tag_v=2; }
    setrf(c,d,fsat(e,B(w,50,1))); break; }
  case O_RRO_R: case O_RRO_C: case O_RRO_I: setrf(c,d,fneg_abs(opBf(c,w,id-O_RRO_R),B(w,45,1),B(w,49,1))); break;
  case O_F2F_R: case O_F2F_C: case O_F2F_I: {
    u32 ds=B(w,8,2), ss=B(w,10,2); char* src;
    if(ss==1){ /* F16 */ src=T("unpackHalf2x16(%s).%c",opB(c,w,id-O_F2F_R),B(w,41,1)?'y':'x'); }
    else src=opBf(c,w,id-O_F2F_R);
    src=fneg_abs(src,B(w,45,1),B(w,49,1));
    if(ds==ss && ss==2){ u32 rm=B(w,39,2); static const char* fn[4]={"roundEven","floor","ceil","trunc"}; if(B(w,42,1)) src=T("%s(%s)",fn[rm],src); else if(rm) src=T("%s(%s)",fn[rm],src); }
    if(ds==1) setr(c,d,T("packHalf2x16(vec2(%s,0.0))",fsat(src,B(w,50,1)))); else setrf(c,d,fsat(src,B(w,50,1)));
    break; }
  case O_F2I_R: case O_F2I_C: case O_F2I_I: {
    char* src=opBf(c,w,id-O_F2I_R);
    if(B(w,10,2)==1) src=T("unpackHalf2x16(%s).%c",opB(c,w,id-O_F2I_R),B(w,41,1)?'y':'x');
    src=fneg_abs(src,B(w,49,1),B(w,45,1));
    static const char* fn[4]={"roundEven","floor","ceil","trunc"}; src=T("%s(%s)",fn[B(w,39,2)],src);
    if(B(w,12,1)) setr(c,d,T("uint(int(clamp(%s,-2147483648.0,2147483520.0)))",src));
    else setr(c,d,T("uint(clamp(%s,0.0,4294967040.0))",src));
    break; }
  case O_I2F_R: case O_I2F_C: case O_I2F_I: {
    char* s=opB(c,w,id-O_I2F_R); u32 fmt=B(w,10,2), sel=B(w,41,2); int sg=B(w,13,1);
    const char* v;
    if(fmt==0) v= sg? T("float(bitfieldExtract(int(%s),%u,8))",s,sel*8) : T("float(bitfieldExtract(%s,%u,8))",s,sel*8);
    else if(fmt==1) v= sg? T("float(bitfieldExtract(int(%s),%u,16))",s,(sel&2)*8) : T("float(bitfieldExtract(%s,%u,16))",s,(sel&2)*8);
    else v= sg? T("float(int(%s))",s) : T("float(%s)",s);
    v=fneg_abs(v,B(w,45,1),B(w,49,1));
    if(B(w,8,2)==1) setr(c,d,T("packHalf2x16(vec2(%s,0.0))",v)); else setrf(c,d,v);
    break; }
  case O_I2I_R: case O_I2I_C: case O_I2I_I: {
    char* s=opB(c,w,id-O_I2I_R); u32 sf=B(w,10,2), df=B(w,8,2), sel=B(w,41,2); int ss=B(w,13,1), ds=B(w,12,1);
    const char* v= sf==0? (ss? T("bitfieldExtract(int(%s),%u,8)",s,sel*8) : T("int(bitfieldExtract(%s,%u,8))",s,sel*8)) :
                   sf==1? (ss? T("bitfieldExtract(int(%s),%u,16)",s,(sel&2)*8) : T("int(bitfieldExtract(%s,%u,16))",s,(sel&2)*8)) : T("int(%s)",s);
    if(B(w,49,1)) v=T("abs(%s)",v); if(B(w,45,1)) v=T("(-%s)",v);
    if(B(w,50,1)){ if(df==0) v= ds? T("clamp(%s,-128,127)",v) : T("clamp(%s,0,255)",v); else if(df==1) v= ds? T("clamp(%s,-32768,32767)",v) : T("clamp(%s,0,65535)",v); }
    else { if(df==0) v= ds? T("bitfieldExtract(%s,0,8)",v) : T("(%s & 255)",v); else if(df==1) v= ds? T("bitfieldExtract(%s,0,16)",v) : T("(%s & 65535)",v); }
    setr(c,d,T("uint(%s)",v)); break; }
  /* ---------------- moves */
  case O_MOV_R: case O_MOV_C: case O_MOV_I: if(d!=255) c->gtag[d]= id==O_MOV_C? gbuf_tag(c,w) : id==O_MOV_R? c->gtag[B(w,20,8)] : 0; setr(c,d,opB(c,w,id-O_MOV_R)); break;
  case O_MOV32I: setr(c,d,imm32(w)); break;
  case O_S2R: {
    u32 sr=B(w,20,8); const char* v="0u";
    switch(sr){ case 0x00: v="0u"; break; case 0x12: v="floatBitsToUint(1.0)"; break;
      case 0x20: v= c->stage==ST_CS? "(gl_LocalInvocationID.x | (gl_LocalInvocationID.y<<16) | (gl_LocalInvocationID.z<<26))" : "0u"; break;
      case 0x21: v= c->stage==ST_CS? "gl_LocalInvocationID.x" : "0u"; break; case 0x22: v= c->stage==ST_CS? "gl_LocalInvocationID.y" : "0u"; break; case 0x23: v= c->stage==ST_CS? "gl_LocalInvocationID.z" : "0u"; break;
      case 0x25: v= c->stage==ST_CS? "gl_WorkGroupID.x" : "0u"; break; case 0x26: v= c->stage==ST_CS? "gl_WorkGroupID.y" : "0u"; break; case 0x27: v= c->stage==ST_CS? "gl_WorkGroupID.z" : "0u"; break;
      case 0x1D: v= c->stage==ST_FS? "0u" : "0x00ff0000u"; break;
      case 0x38: v="1u"; break; case 0x39: v="0u"; break; case 0x3A: v="1u"; break; case 0x3B: v="0xFFFFFFFEu"; break; case 0x3C: v="0xFFFFFFFFu"; break;
      case 0x50: case 0x52: v="uint(0)"; break; default: v="0u"; }
    setr(c,d,v); break; }
  case O_CS2R: setr(c,d,"0u"); if(d!=255) setr(c,d+1,"0u"); break;
  /* ---------------- integer */
  case O_IADD_R: case O_IADD_C: case O_IADD_I: {
    if(d!=255){ u8 tg=0; if(id==O_IADD_C) tg=gbuf_tag(c,w); if(!tg) tg=c->gtag[a]; c->gtag[d]=tg; }
    char* A=R(c,a); char* Bv=opB(c,w,id-O_IADD_R); if(B(w,49,1)) A=T("(0u-%s)",A); if(B(w,48,1)) Bv=T("(0u-%s)",Bv);
    const char* e=T("(%s + %s)",A,Bv); if(B(w,43,1)) e=T("(%s + (cc_c?1u:0u))",e);
    if(B(w,47,1)){ c->uses_cc=1; emitl(c,"{ uint t_ = %s; uint c1_; uint s_ = uaddCarry(%s,%s,c1_); uint c2_=0u; %s cc_c = (c1_|c2_)!=0u; cc_z = t_==0u; cc_s = int(t_)<0; t_=s_%s; %s }",
        e,A,Bv, B(w,43,1)? "s_ = uaddCarry(s_, cc_c?1u:0u, c2_);" : "", "", d!=255? T("r%u = t_;",d) : ""); c->used_reg[d]=1; }
    else setr(c,d,e);
    break; }
  case O_IADD32I: { char* A=R(c,a); if(B(w,56,1)) A=T("(0u-%s)",A); const char* e=T("(%s + %s)",A,imm32(w)); if(B(w,53,1)) e=T("(%s + (cc_c?1u:0u))",e);
    if(B(w,52,1)){ c->uses_cc=1; emitl(c,"{ uint c1_; uint s_ = uaddCarry(%s,%s,c1_); cc_c = c1_!=0u; cc_z = s_==0u; cc_s=int(s_)<0; }",A,imm32(w)); }
    setr(c,d,e); break; }
  case O_IADD3_R: case O_IADD3_C: case O_IADD3_I: {
    char* A=R(c,a); char* Bv= id==O_IADD3_R? R(c,B(w,20,8)) : id==O_IADD3_C? CB(c,cb_idx(w),cb_off(w)) : imm20i(w); char* Cv=R(c,B(w,39,8));
    if(id==O_IADD3_R){ static const char* hs[4]={"%s","(%s & 0xFFFFu)","(%s >> 16)","%s"}; A=T(hs[B(w,35,2)],A); Bv=T(hs[B(w,33,2)],Bv); Cv=T(hs[B(w,31,2)],Cv); }
    if(B(w,51,1)) A=T("(0u-%s)",A); if(B(w,50,1)) Bv=T("(0u-%s)",Bv); if(B(w,49,1)) Cv=T("(0u-%s)",Cv);
    const char* ab=T("(%s + %s)",A,Bv);
    if(id==O_IADD3_R){ u32 sh=B(w,37,2); if(sh==1) ab=T("(%s >> 16)",ab); else if(sh==2) ab=T("(%s << 16)",ab); }
    setr(c,d,T("(%s + %s)",ab,Cv)); break; }
  case O_ISCADD_R: case O_ISCADD_C: case O_ISCADD_I: {
    char* A=R(c,a); char* Bv=opB(c,w,id-O_ISCADD_R); if(B(w,49,1)) A=T("(0u-%s)",A); if(B(w,48,1)) Bv=T("(0u-%s)",Bv);
    setr(c,d,T("((%s << %uu) + %s)",A,B(w,39,5),Bv)); break; }
  case O_ISCADD32I: setr(c,d,T("((%s << %uu) + %s)",R(c,a),B(w,53,5),imm32(w))); break;
  case O_IMNMX_R: case O_IMNMX_C: case O_IMNMX_I: {
    char* A=R(c,a); char* Bv=opB(c,w,id-O_IMNMX_R); const char* pr=PN(c,B(w,39,3),B(w,42,1));
    if(B(w,48,1)) setr(c,d,T("uint(%s ? min(int(%s),int(%s)) : max(int(%s),int(%s)))",pr,A,Bv,A,Bv));
    else setr(c,d,T("(%s ? min(%s,%s) : max(%s,%s))",pr,A,Bv,A,Bv)); break; }
  case O_ISETP_R: case O_ISETP_C: case O_ISETP_I: {
    const char* t=icmp_expr(B(w,49,3),R(c,a),opB(c,w,id-O_ISETP_R),B(w,48,1)); const char* pc=PN(c,B(w,39,3),B(w,42,1));
    setp2(c,B(w,3,3),B(w,0,3),B(w,45,2),t,pc); break; }
  case O_ISET_R: case O_ISET_C: case O_ISET_I: {
    const char* t=bop(B(w,45,2),icmp_expr(B(w,49,3),R(c,a),opB(c,w,id-O_ISET_R),B(w,48,1)),PN(c,B(w,39,3),B(w,42,1)));
    setr(c,d,T("(%s ? %s : 0u)",t,B(w,44,1)? "floatBitsToUint(1.0)" : "0xFFFFFFFFu")); break; }
  case O_ICMP_R: case O_ICMP_RC: case O_ICMP_CR: case O_ICMP_I: {
    char *Bv,*Cv;
    if(id==O_ICMP_R){ Bv=R(c,B(w,20,8)); Cv=R(c,B(w,39,8)); } else if(id==O_ICMP_RC){ Bv=R(c,B(w,39,8)); Cv=CB(c,cb_idx(w),cb_off(w)); }
    else if(id==O_ICMP_CR){ Bv=CB(c,cb_idx(w),cb_off(w)); Cv=R(c,B(w,39,8)); } else { Bv=imm20i(w); Cv=R(c,B(w,39,8)); }
    setr(c,d,T("(%s ? %s : %s)",icmp_expr(B(w,49,3),Cv,"0u",B(w,48,1)),R(c,a),Bv)); break; }
  case O_LOP_R: case O_LOP_C: case O_LOP_I: case O_LOP32I: {
    char* A=R(c,a); char* Bv= id==O_LOP32I? imm32(w) : opB(c,w,id-O_LOP_R);
    u32 opn= id==O_LOP32I? B(w,53,2) : B(w,41,2); int ia= id==O_LOP32I? B(w,55,1) : B(w,39,1); int ib= id==O_LOP32I? B(w,56,1) : B(w,40,1);
    if(ia) A=T("(~%s)",A); if(ib) Bv=T("(~%s)",Bv);
    const char* e= opn==0? T("(%s & %s)",A,Bv) : opn==1? T("(%s | %s)",A,Bv) : opn==2? T("(%s ^ %s)",A,Bv) : Bv;
    if(id!=O_LOP32I && B(w,48,3)!=7){ u32 po=B(w,44,2); emitl(c,"{ uint t_ = %s;",e); c->indent++; setr(c,d,"t_");
      setp(c,B(w,48,3), po==0? "false" : po==1? "true" : po==2? "(t_ == 0u)" : "(t_ != 0u)"); c->indent--; emitl(c,"}"); }
    else setr(c,d,e);
    if((id==O_LOP32I? B(w,52,1) : B(w,47,1))){ c->uses_cc=1; emitl(c,"cc_z = (%s) == 0u; cc_s = int(%s) < 0;",R(c,d),R(c,d)); }
    break; }
  case O_LOP3_R: case O_LOP3_C: case O_LOP3_I: {
    u32 lut= id==O_LOP3_R? B(w,28,8) : B(w,48,8);
    char* A=R(c,a); char* Bv= id==O_LOP3_R? R(c,B(w,20,8)) : id==O_LOP3_C? CB(c,cb_idx(w),cb_off(w)) : imm20i(w); char* Cv=R(c,B(w,39,8));
    Sb e={0}; sb_put(&e,"(0u");
    for(int i=0;i<8;i++) if(lut&(1u<<i)) sb_f(&e," | (%s%s & %s%s & %s%s)",(i&4)?"":"~",A,(i&2)?"":"~",Bv,(i&1)?"":"~",Cv);
    sb_put(&e,")");
    if(id==O_LOP3_R && B(w,48,3)!=7){ u32 po=B(w,36,2); emitl(c,"{ uint t_ = %s;",e.p); c->indent++; setr(c,d,"t_");
      setp(c,B(w,48,3), po==0? "false" : po==1? "true" : po==2? "(t_ == 0u)" : "(t_ != 0u)"); c->indent--; emitl(c,"}"); }
    else setr(c,d,e.p);
    free(e.p); break; }
  case O_SHL_R: case O_SHL_C: case O_SHL_I: {
    char* Bv=opB(c,w,id-O_SHL_R);
    if(B(w,39,1)) setr(c,d,T("(%s << (%s & 31u))",R(c,a),Bv)); else setr(c,d,T("((%s) >= 32u ? 0u : (%s << (%s)))",Bv,R(c,a),Bv)); break; }
  case O_SHR_R: case O_SHR_C: case O_SHR_I: {
    char* Bv=opB(c,w,id-O_SHR_R); char* A=R(c,a); if(B(w,40,1)) A=T("bitfieldReverse(%s)",A);
    const char* sh= B(w,39,1)? T("(%s & 31u)",Bv) : T("min(%s,31u)",Bv);
    if(B(w,48,1)) setr(c,d,T("uint(int(%s) >> int(%s))",A,sh));
    else setr(c,d, B(w,39,1)? T("(%s >> %s)",A,sh) : T("((%s) >= 32u ? 0u : (%s >> (%s)))",Bv,A,Bv));
    break; }
  case O_SHF_LR: case O_SHF_LI: case O_SHF_RR: case O_SHF_RI: {
    char* lo=R(c,a); char* sh= (id==O_SHF_LR||id==O_SHF_RR)? R(c,B(w,20,8)) : T("%uu",B(w,20,6)); char* hi=R(c,B(w,39,8));
    int left= id==O_SHF_LR||id==O_SHF_LI;
    const char* s=T("(%s & 63u)",sh);
    if(left) setr(c,d,T("(%s >= 32u ? (%s << (%s-32u)) : (%s == 0u ? %s : ((%s << %s) | (%s >> (32u-%s)))))",s,lo,s,s,hi,hi,s,lo,s));
    else setr(c,d,T("(%s >= 32u ? (%s >> (%s-32u)) : (%s == 0u ? %s : ((%s >> %s) | (%s << (32u-%s)))))",s,hi,s,s,lo,lo,s,hi,s));
    break; }
  case O_BFE_R: case O_BFE_C: case O_BFE_I: {
    char* Bv=opB(c,w,id-O_BFE_R); char* A=R(c,a); if(B(w,40,1)) A=T("bitfieldReverse(%s)",A);
    const char* off=T("int(%s & 255u)",Bv), *cnt=T("int((%s >> 8) & 255u)",Bv);
    if(B(w,48,1)) setr(c,d,T("uint((%s)==0 ? 0 : bitfieldExtract(int(%s), min(%s,31), min(%s,32-min(%s,31))))",cnt,A,off,cnt,off));
    else setr(c,d,T("((%s)==0 ? 0u : bitfieldExtract(%s, min(%s,31), min(%s,32-min(%s,31))))",cnt,A,off,cnt,off));
    break; }
  case O_BFI_R: case O_BFI_RC: case O_BFI_CR: case O_BFI_I: {
    char *Bv,*Cv;
    if(id==O_BFI_R){ Bv=R(c,B(w,20,8)); Cv=R(c,B(w,39,8)); } else if(id==O_BFI_RC){ Bv=R(c,B(w,39,8)); Cv=CB(c,cb_idx(w),cb_off(w)); }
    else if(id==O_BFI_CR){ Bv=CB(c,cb_idx(w),cb_off(w)); Cv=R(c,B(w,39,8)); } else { Bv=imm20i(w); Cv=R(c,B(w,39,8)); }
    const char* off=T("int(%s & 255u)",Bv), *cnt=T("int((%s >> 8) & 255u)",Bv);
    setr(c,d,T("bitfieldInsert(%s, %s, min(%s,31), min(%s,32-min(%s,31)))",Cv,R(c,a),off,cnt,off)); break; }
  case O_SEL_R: case O_SEL_C: case O_SEL_I: setr(c,d,T("(%s ? %s : %s)",PN(c,B(w,39,3),B(w,42,1)),R(c,a),opB(c,w,id-O_SEL_R))); break;
  case O_POPC_R: case O_POPC_C: case O_POPC_I: { char* Bv=opB(c,w,id-O_POPC_R); if(B(w,40,1)) Bv=T("(~%s)",Bv); setr(c,d,T("uint(bitCount(%s))",Bv)); break; }
  case O_FLO_R: case O_FLO_C: case O_FLO_I: { char* Bv=opB(c,w,id-O_FLO_R); if(B(w,40,1)) Bv=T("(~%s)",Bv);
    const char* m= B(w,48,1)? T("findMSB(int(%s))",Bv) : T("findMSB(%s)",Bv);
    if(B(w,41,1)) setr(c,d,T("(%s < 0 ? 0xFFFFFFFFu : uint(31 - %s))",m,m)); else setr(c,d,T("uint(%s)",m)); break; }
  case O_PRMT_R: case O_PRMT_RC: case O_PRMT_CR: case O_PRMT_I: {
    char *Bv,*Cv;
    if(id==O_PRMT_R){ Bv=R(c,B(w,20,8)); Cv=R(c,B(w,39,8)); } else if(id==O_PRMT_RC){ Bv=R(c,B(w,39,8)); Cv=CB(c,cb_idx(w),cb_off(w)); }
    else if(id==O_PRMT_CR){ Bv=CB(c,cb_idx(w),cb_off(w)); Cv=R(c,B(w,39,8)); } else { Bv=imm20i(w); Cv=R(c,B(w,39,8)); }
    emitl(c,"{ uint s_ = %s; uint a_ = %s, c_ = %s; uint o_ = 0u;",Bv,R(c,a),Cv); c->indent++;
    emitl(c,"for(int i=0;i<4;i++){ uint sel=(s_>>(i*4))&15u; uint src=(sel&4u)!=0u? c_ : a_; uint by=(src>>((sel&3u)*8u))&255u; if((sel&8u)!=0u) by=(by&128u)!=0u?255u:0u; o_ |= by<<(i*8); }");
    setr(c,d,"o_"); c->indent--; emitl(c,"}"); break; }
  case O_XMAD_R: case O_XMAD_RC: case O_XMAD_CR: case O_XMAD_I: {
    char *Bv,*Cv; u32 mode, hb; int psl=0, mrg=0;
    if(id==O_XMAD_R){ Bv=R(c,B(w,20,8)); Cv=R(c,B(w,39,8)); mode=B(w,50,3); hb=B(w,35,1); psl=B(w,36,1); mrg=B(w,37,1); }
    else if(id==O_XMAD_RC){ Bv=R(c,B(w,39,8)); Cv=CB(c,cb_idx(w),cb_off(w)); mode=B(w,50,2); hb=B(w,52,1); }
    else if(id==O_XMAD_CR){ Bv=CB(c,cb_idx(w),cb_off(w)); Cv=R(c,B(w,39,8)); mode=B(w,50,2); hb=B(w,52,1); psl=B(w,55,1); mrg=B(w,56,1); }   /* .PSL/.MRG live at 55/56 in this form */
    else { Bv=T("%uu",B(w,20,16)); Cv=R(c,B(w,39,8)); mode=B(w,50,3); hb=0; psl=B(w,36,1); mrg=B(w,37,1); }
    int sa=B(w,48,1), sb=B(w,49,1), ha=B(w,53,1);
    const char* A= sa? T("uint(bitfieldExtract(int(%s),%d,16))",R(c,a),ha?16:0) : T("bitfieldExtract(%s,%d,16)",R(c,a),ha?16:0);
    const char* Bb= sb? T("uint(bitfieldExtract(int(%s),%d,16))",Bv,hb?16:0) : T("bitfieldExtract(%s,%d,16)",Bv,hb?16:0);
    const char* C= mode==1? T("(%s & 0xFFFFu)",Cv) : mode==2? T("(%s >> 16)",Cv) : mode==4? T("((%s << 16) + %s)",Bv,Cv) : Cv;
    const char* prod=T("(%s * %s)",A,Bb); if(psl) prod=T("(%s << 16)",prod);
    const char* e=T("(%s + %s)",prod,C);
    if(mrg) e=T("((%s & 0xFFFFu) | (%s << 16))",e,Bv);
    if(B(w,47,1)){ c->uses_cc=1; emitl(c,"{ uint t_=%s; cc_z = t_==0u; cc_s = int(t_)<0; uint k_; uaddCarry(%s,%s,k_); cc_c = k_!=0u; }",e,prod,C); }
    setr(c,d,e); break; }
  case O_IMUL_R: case O_IMUL_C: case O_IMUL_I: case O_IMUL32I: {
    char* Bv= id==O_IMUL32I? imm32(w) : opB(c,w,id-O_IMUL_R); int hi= id==O_IMUL32I? B(w,53,1) : B(w,39,1);
    int sgn= id==O_IMUL32I? B(w,54,1) : B(w,40,1);
    if(hi){ if(sgn) emitl(c,"{ int h_, l_; imulExtended(int(%s),int(%s),h_,l_); %s }",R(c,a),Bv,d!=255? T("r%u = uint(h_);",d) : "");
            else emitl(c,"{ uint h_, l_; umulExtended(%s,%s,h_,l_); %s }",R(c,a),Bv,d!=255? T("r%u = h_;",d) : ""); if(d!=255) c->used_reg[d]=1; }
    else setr(c,d,T("(%s * %s)",R(c,a),Bv)); break; }
  case O_IMAD_R: case O_IMAD_RC: case O_IMAD_CR: case O_IMAD_I: case O_IMAD32I: {
    char *Bv,*Cv;
    if(id==O_IMAD_R){ Bv=R(c,B(w,20,8)); Cv=R(c,B(w,39,8)); } else if(id==O_IMAD_RC){ Bv=R(c,B(w,39,8)); Cv=CB(c,cb_idx(w),cb_off(w)); }
    else if(id==O_IMAD_CR){ Bv=CB(c,cb_idx(w),cb_off(w)); Cv=R(c,B(w,39,8)); } else if(id==O_IMAD_I){ Bv=imm20i(w); Cv=R(c,B(w,39,8)); } else { Bv=imm32(w); Cv=R(c,d); }
    /* IMAD.HI (bit 54, A signed @48, B signed @53): hi32(a*b) + c. this is how div-by-constant gets compiled */
    if(id!=O_IMAD32I && B(w,54,1)){
      const char* cv=Cv; if(d!=255){
        if(B(w,48,1)||B(w,53,1)) emitl(c,"{ int h_, l_; imulExtended(int(%s),int(%s),h_,l_); r%u = uint(h_) + %s; }",R(c,a),Bv,d,cv);
        else emitl(c,"{ uint h_, l_; umulExtended(%s,%s,h_,l_); r%u = h_ + %s; }",R(c,a),Bv,d,cv);
        c->used_reg[d]=1; } break; }
    setr(c,d,T("(%s * %s + %s)",R(c,a),Bv,Cv)); break; }
  case O_LEA_LR: case O_LEA_LC: case O_LEA_LI: case O_LEA_HR: case O_LEA_HC: {
    char* Bv= id==O_LEA_LR||id==O_LEA_HR? R(c,B(w,20,8)) : id==O_LEA_LI? imm20i(w) : CB(c,cb_idx(w),cb_off(w));
    u32 sh= (id==O_LEA_LR||id==O_LEA_LI||id==O_LEA_LC)? B(w,39,5) : B(w,28,5);
    char* A=R(c,a); if(B(w,45,1)) A=T("(0u-%s)",A);
    if(id==O_LEA_HR||id==O_LEA_HC){ char* hi=R(c,B(w,39,8)); setr(c,d,T("((%s >> (32u-%uu)) | (%s << %uu)) + %s",A,sh?sh:32,hi,sh,Bv)); }
    else setr(c,d,T("((%s << %uu) + %s)",A,sh,Bv));
    if(B(w,48,3)!=7 && (id==O_LEA_LR||id==O_LEA_LI||id==O_LEA_LC)) setp(c,B(w,48,3),"false");
    break; }
  /* ---------------- predicates */
  case O_PSETP: {
    const char* ab=bop(B(w,24,2),PN(c,B(w,12,3),B(w,15,1)),PN(c,B(w,29,3),B(w,32,1))); const char* cc=PN(c,B(w,39,3),B(w,42,1));
    setp2(c,B(w,3,3),B(w,0,3),B(w,45,2),ab,cc); break; }
  case O_PSET: {
    const char* ab=bop(B(w,24,2),PN(c,B(w,12,3),B(w,15,1)),PN(c,B(w,29,3),B(w,32,1)));
    const char* t=bop(B(w,45,2),ab,PN(c,B(w,39,3),B(w,42,1)));
    setr(c,d,T("(%s ? %s : 0u)",t,B(w,44,1)? "floatBitsToUint(1.0)" : "0xFFFFFFFFu")); break; }
  case O_P2R_R: case O_P2R_C: case O_P2R_I: {
    char* M=opB(c,w,id-O_P2R_R); u32 sh=B(w,41,2)*8;
    Sb e={0}; sb_put(&e,"(0u"); for(int i=0;i<7;i++) sb_f(&e," | (%s ? %du : 0u)",P(c,i),1<<i); sb_put(&e,")");
    setr(c,d,T("((%s & ~(%s << %uu)) | ((%s & %s) << %uu))",R(c,a),M,sh,e.p,M,sh)); free(e.p); break; }
  case O_R2P_R: case O_R2P_C: case O_R2P_I: {
    char* M=opB(c,w,id-O_R2P_R); u32 sh=B(w,41,2)*8;
    emitl(c,"{ uint v_ = %s >> %uu; uint m_ = %s;",R(c,a),sh,M); c->indent++;
    for(int i=0;i<7;i++) emitl(c,"if((m_ & %du) != 0u) %s = (v_ & %du) != 0u;",1<<i,P(c,i),1<<i);
    c->indent--; emitl(c,"}"); break; }
  case O_CSETP: { /* condition on CC */
    c->uses_cc=1; u32 ft=B(w,8,5); const char* t= ft==15? "true" : ft==2||ft==10? "cc_z" : ft==5||ft==13? "!cc_z" : ft==1? "cc_s" : ft==6? "!cc_s" : "true";
    const char* pc=PN(c,B(w,39,3),B(w,42,1)); setp(c,B(w,3,3),bop(B(w,45,2),t,pc)); setp(c,B(w,0,3),bop(B(w,45,2),T("(!%s)",t),pc)); break; }
  /* ---------------- attribs + memory */
  case O_IPA: {
    u32 addr=B(w,30,8)*4; const char* v=attr_read(c,addr,0);
    if(B(w,38,1)) v="0u";
    /* hw semantics: perspective generics come out pre-multiplied by w (1/gl_FragCoord.w). PASS keeps that,
       MULTIPLY multiplies by the given reg */
    else if(c->stage==ST_FS && addr>=0x80 && addr<0x280){ u32 gi=(addr-0x80)>>4, cp=(addr>>2)&3;
      u32 im=(c->si->sph[6+gi/4]>>((gi%4)*8+cp*2))&3; u32 mode=B(w,54,2);
      /* IPA.MULTIPLY by RCP(IPA pos.w) cancels out, just read the varying */
      if(im==2 && mode==1 && c->wtag[B(w,20,8)]==2 && B(w,20,8)!=255){ }
      else {
      if(im==2) v=T("floatBitsToUint(uintBitsToFloat(%s)*(1.0/gl_FragCoord.w))",v);
      if(mode==1) v=T("floatBitsToUint(uintBitsToFloat(%s)*uintBitsToFloat(%s))",v,R(c,B(w,20,8))); } }
    else if(c->stage==ST_FS && addr==0x7C && B(w,54,2)==0 && !B(w,51,1)){ c->tag_r=(int)d; c->tag_v=1; }
    if(B(w,51,1)) v=T("floatBitsToUint(clamp(uintBitsToFloat(%s),0.0,1.0))",v);
    setr(c,d,v); break; }
  case O_ALD: {
    u32 addr=B(w,20,10), n=B(w,47,2)+1; int is_out=B(w,32,1);
    if(B(w,8,8)!=255){ /* indexed attribs: only generic-by-register */
      for(u32 k=0;k<n;k++) setr(c,d+k,"0u"); unsupported(c,"indirect ALD",w); break; }
    for(u32 k=0;k<n;k++) setr(c,d+k,attr_read(c,addr+k*4,is_out));
    break; }
  case O_AST: {
    u32 addr=B(w,20,10), n=B(w,47,2)+1; u32 ra=B(w,8,8);
    if(ra!=255){ /* runtime-indexed output */
      emitl(c,"{ uint aa_ = %s + %uu;",R(c,ra),addr); c->indent++;
      for(u32 k=0;k<n;k++){ const char* v=R(c,d+k); emitl(c,"switch(aa_ + %uu){",k*4); c->indent++;
        for(u32 q=0;q<4;q++) emitl(c,"case %uu: gl_Position.%c = uintBitsToFloat(%s); break;",0x70+q*4,"xyzw"[q],v);
        for(u32 i=0;i<16;i++){ c->out_used[i]=1; for(u32 q=0;q<4;q++) emitl(c,"case %uu: out_attr%u.%c = uintBitsToFloat(%s); break;",0x80+i*16+q*4,i,"xyzw"[q],v); }
        emitl(c,"default: break;"); c->indent--; emitl(c,"}"); }
      c->indent--; emitl(c,"}"); break; }
    for(u32 k=0;k<n;k++) attr_write(c,addr+k*4,R(c,d+k)); break; }
  case O_LDC: {
    u32 idx=B(w,36,5); i32 off=(i32)(B(w,20,16)<<16)>>16; u32 sz=B(w,48,3); u32 ar=a;
    c->si->cbuf_mask|=1u<<idx; if(idx<18) c->si->cbuf_max[idx]=65536;
    const char* addr= ar==255? T("%uu",(u32)off) : T("(%s + %uu)",R(c,ar),(u32)off);
    if(ar!=255) c->si->cbuf_indirect_mask|=1u<<idx;
    /* compute the address first, dst can alias the address reg */
    emitl(c,"{ uint ca_ = %s;",addr); c->indent++; addr="ca_";
    const char* word=T("floatBitsToUint(cb%u.d[(%s)>>4][((%s)>>2)&3u])",idx,addr,addr);
    if(sz==4) setr(c,d,word);
    else if(sz==5){ setr(c,d,word); setr(c,d+1,T("floatBitsToUint(cb%u.d[((%s)+4u)>>4][(((%s)+4u)>>2)&3u])",idx,addr,addr)); }
    else if(sz==0) setr(c,d,T("bitfieldExtract(%s, int(((%s)&3u)*8u), 8)",word,addr));
    else if(sz==1) setr(c,d,T("uint(bitfieldExtract(int(%s), int(((%s)&3u)*8u), 8))",word,addr));
    else if(sz==2) setr(c,d,T("bitfieldExtract(%s, int(((%s)&2u)*8u), 16)",word,addr));
    else if(sz==3) setr(c,d,T("uint(bitfieldExtract(int(%s), int(((%s)&2u)*8u), 16))",word,addr));
    else setr(c,d,word);
    c->indent--; emitl(c,"}");
    break; }
  case O_LDS: case O_STS: case O_LDL: case O_STL: {
    int shared= id==O_LDS||id==O_STS; int load= id==O_LDS||id==O_LDL;
    i32 off=(i32)(B(w,20,24)<<8)>>8; u32 sz=B(w,48,3); const char* mem= shared? "smem" : "lmem";
    if(shared) c->uses_shared=1; else c->uses_local=1;
    const char* addr= a==255? T("%uu",(u32)off) : T("(%s + %uu)",R(c,a),(u32)off);
    u32 n= sz==5? 2 : sz==6? 4 : 1;
    emitl(c,"{ uint la_ = %s;",addr); c->indent++; addr="la_";
    for(u32 k=0;k<n;k++){ const char* ix=T("(((%s)>>2)+%uu) & %su",addr,k,shared? "0xFFFF" : "0x3FF");
      if(load){ if(sz<4) setr(c,d+k,T("bitfieldExtract(%s[%s], int(((%s)&3u)*8u), %d)",mem,ix,addr,sz<2?8:16)); else setr(c,d+k,T("%s[%s]",mem,ix)); }
      else emitl(c,"%s[%s] = %s;",mem,ix,R(c,d+k)); }
    c->indent--; emitl(c,"}");
    break; }
  case O_LDG: if(c->gtag[a]){
    /* ssbo load, addr = c0[off] + offset */
    int gi=c->gtag[a]-1; u32 goff=c->si->gbuf_cbuf_off[gi]; i32 imm=(i32)(B(w,20,24)<<8)>>8; u32 sz=B(w,48,3);
    c->si->uses_global=1;
    emitl(c,"{ uint o_ = %s + %uu - %s;",R(c,a),(u32)imm,CB(c,0,goff)); c->indent++;
    if(sz<=3){ const char* v=T("((g%d.d[o_>>2u] >> ((o_&3u)*8u)) & %s)",gi, sz<=1? "0xFFu" : "0xFFFFu");
      if(sz==1) v=T("uint(int(%s<<24u)>>24)",v); else if(sz==3) v=T("uint(int(%s<<16u)>>16)",v); setr(c,d,v); }
    else { u32 n= sz==5? 2 : sz>=6? 4 : 1; for(u32 k=0;k<n;k++) setr(c,d+k,T("g%d.d[(o_>>2u)+%uu]",gi,k)); }
    c->indent--; emitl(c,"}"); break; }
    /* unknown base -> unsupported */
  case O_STG: if(id==O_STG && c->gtag[a]){
    /* ssbo store: 32/64/128 direct, 8/16 via atomics so neighbours survive */
    int gi=c->gtag[a]-1; u32 goff=c->si->gbuf_cbuf_off[gi]; i32 imm=(i32)(B(w,20,24)<<8)>>8; u32 sz=B(w,48,3);
    c->si->uses_global=1; c->si->gbuf_wmask|=1u<<gi;
    emitl(c,"{ uint o_ = %s + %uu - %s;",R(c,a),(u32)imm,CB(c,0,goff)); c->indent++;
    if(sz<=3){ const char* m= sz<=1? "0xFFu" : "0xFFFFu"; emitl(c,"uint s_=(o_&3u)*8u; atomicAnd(g%d.d[o_>>2u], ~(%s<<s_)); atomicOr(g%d.d[o_>>2u], (%s & %s)<<s_);",gi,m,gi,R(c,d),m); }
    else { u32 n= sz==5? 2 : sz>=6? 4 : 1; for(u32 k=0;k<n;k++) emitl(c,"g%d.d[(o_>>2u)+%uu] = %s;",gi,k,R(c,d+k)); }
    c->indent--; emitl(c,"}"); break; }
  {
    c->si->uses_global=1; unsupported(c, id==O_LDG? "LDG" : "STG", w);
    if(id==O_LDG){ u32 sz=B(w,48,3), n= sz==5? 2 : sz>=6? 4 : 1; for(u32 k=0;k<n;k++) setr(c,d+k,"0u"); }
    break; }
  /* ---------------- textures */
  case O_TEXS: {
    u32 enc=B(w,53,4), ra=a, rb=B(w,20,8); u32 off=B(w,36,13);
    int type=1, array=0, shadow=0; const char* crd="vec2(0.0)"; const char* lod=NULL; const char* dref=NULL; const char* arr=NULL;
    switch(enc){
      case 0: type=0; crd=RF(c,ra); lod="0.0"; break;
      case 1: crd=T("vec2(%s,%s)",RF(c,ra),RF(c,rb)); break;
      case 2: crd=T("vec2(%s,%s)",RF(c,ra),RF(c,rb)); lod="0.0"; break;
      case 3: crd=T("vec2(%s,%s)",RF(c,ra),RF(c,ra+1)); lod=RF(c,rb); break;
      case 4: shadow=1; crd=T("vec2(%s,%s)",RF(c,ra),RF(c,ra+1)); dref=RF(c,rb); break;
      case 5: shadow=1; crd=T("vec2(%s,%s)",RF(c,ra),RF(c,ra+1)); lod=RF(c,rb); dref=RF(c,rb+1); break;
      case 6: shadow=1; crd=T("vec2(%s,%s)",RF(c,ra),RF(c,ra+1)); dref=RF(c,rb); lod="0.0"; break;
      case 7: array=1; arr=R(c,ra); crd=T("vec2(%s,%s)",RF(c,ra+1),RF(c,rb)); break;
      case 8: array=1; arr=R(c,ra); crd=T("vec2(%s,%s)",RF(c,ra+1),RF(c,rb)); lod="0.0"; break;
      case 9: array=1; shadow=1; arr=R(c,ra); crd=T("vec2(%s,%s)",RF(c,ra+1),RF(c,rb)); dref=RF(c,rb+1); lod="0.0"; break;
      case 10: type=2; crd=T("vec3(%s,%s,%s)",RF(c,ra),RF(c,ra+1),RF(c,rb)); break;
      case 11: type=2; crd=T("vec3(%s,%s,%s)",RF(c,ra),RF(c,ra+1),RF(c,rb)); lod="0.0"; break;
      case 12: type=3; crd=T("vec3(%s,%s,%s)",RF(c,ra),RF(c,ra+1),RF(c,rb)); break;
      case 13: type=3; crd=T("vec3(%s,%s,%s)",RF(c,ra),RF(c,ra+1),RF(c,rb)); lod=RF(c,rb+1); break;   /* cube + LL: z in rb, lod in rb+1 (matches what NVN's compiler emits) */
    }
    int s=tex_slot(c,off,type,array,shadow,0);
    if(type==0) crd=T("vec2(%s, 0.5)",crd);
    if(array) crd=T("vec3(%s, float(%s))",crd,arr);
    const char* smp;
    if(shadow){ const char* cc= array? T("vec4(%s,%s)",crd,dref) : T("vec3(%s,%s)",crd,dref);
      smp= lod? T("vec4(textureLod(tex%d, %s, %s))",s, array? T("%s",cc) : cc, lod) : T("vec4(texture(tex%d, %s))",s,cc);
      if(lod && !array) smp=T("vec4(textureLod(tex%d, vec3(%s,%s), %s))",s,crd,dref,lod);
      if(array) smp=T("vec4(texture(tex%d, vec4(%s,%s)))",s,crd,dref); }
    else smp= lod? T("textureLod(tex%d, %s, %s)",s,crd,lod) : T("texture(tex%d, %s)",s,crd);
    emitl(c,"{ vec4 t_ = %s;",IW(c,s,smp)); c->indent++; write_texs(c,w,"t_",!B(w,59,1)); c->indent--; emitl(c,"}");   /* bit59: fp32 */
    break; }
  case O_SULD: case O_SUST: {
    /* compute images. SUST -> imageStore, SULD -> texelFetch on the same texture.
       .P = formatted (mask 20..23), .D = raw bytes (size 20..22) converted using the format class
       uniforms swr_ic / swr_tc the runtime sets at dispatch */
    u32 isd=B(w,52,1), bound=B(w,51,1), ty=B(w,33,3), ra=a, rdd=d;
    if(c->stage!=ST_CS || ty==1 || ty>5){ unsupported(c, id==O_SUST? "SUST" : "SULD", w); if(id==O_SULD && rdd!=255){ setr(c,rdd,"0u"); } break; }
    u32 off=0, bank=0; int bl=0;
    if(bound) off=B(w,36,13);
    else { u32 bo; if(!track_handle(c,idx,B(w,39,8),&bank,&bo)){ unsupported(c, id==O_SUST? "SUST.B (untracked handle)" : "SULD.B (untracked handle)", w); if(id==O_SULD) setr(c,rdd,"0u"); break; } off=bo/4; bl=1; }
    const char* crd;
    switch(ty){ case 0: crd=T("int(%s)",R(c,ra)); break; case 2: crd=T("ivec2(int(%s), int(%s))",R(c,ra),R(c,ra+1)); break;
      case 3: crd=T("ivec2(int(%s), int(%s))",R(c,ra),R(c,ra+1)); break; default: crd=T("ivec3(int(%s), int(%s), int(%s))",R(c,ra),R(c,ra+1),R(c,ra+2)); }
    int ba= isd && B(w,23,1);   /* x is in bytes, divide by texel size */
    u32 sz=B(w,20,3), nw= isd? (sz==5? 2 : sz==6? 4 : 1) : 0;
    if(id==O_SUST){
      ShaderInfo* si=c->si; int k=0; for(;k<si->nimg;k++) if(si->img[k].cbuf_off==off && si->img[k].type==ty && si->img[k].bindless==bl && (!bl || si->img[k].bl_cbuf==bank)) break;
      if(k==si->nimg){ if(si->nimg>=8){ unsupported(c,"SUST (too many images)",w); break; } si->img[k].cbuf_off=off; si->img[k].type=(u8)ty; si->img[k].bindless=(u8)bl; si->img[k].bl_cbuf=(u8)bank; si->nimg++;
        if(bl && bank<18){ si->cbuf_mask|=1u<<bank; if(off*4+16>si->cbuf_max[bank]) si->cbuf_max[bank]=((off*4)|15)+1; } }
      if(ba){ c->uses_img_raw=1; crd= ty==0||ty==2? T("int(%s) / swr_ib[%d]",R(c,ra),k) : ty==3? T("ivec2(int(%s) / swr_ib[%d], int(%s))",R(c,ra),k,R(c,ra+1)) : T("ivec3(int(%s) / swr_ib[%d], int(%s), int(%s))",R(c,ra),k,R(c,ra+1),R(c,ra+2));
        if(ty==2) crd=T("ivec2(int(%s) / swr_ib[%d], int(%s))",R(c,ra),k,R(c,ra+1)); }
      if(!isd){ u32 m=B(w,20,4), r=rdd; const char* cc[4]={"0.0","0.0","0.0","1.0"};
        for(int q=0;q<4;q++) if(m&(1u<<q)){ cc[q]= r==255? "0.0" : T("uintBitsToFloat(%s)",R(c,r)); if(r!=255) r++; }
        emitl(c,"imageStore(img%d, %s, vec4(%s, %s, %s, %s));",k,crd,cc[0],cc[1],cc[2],cc[3]); }
      else { c->uses_img_raw=1;
        emitl(c,"imageStore(img%d, %s, swr_r2v(uvec4(%s, %s, %s, %s), %uu, %uu, swr_ic[%d]));",k,crd,R(c,rdd),nw>1? R(c,rdd+1) : "0u",nw>2? R(c,rdd+2) : "0u",nw>2? R(c,rdd+3) : "0u",nw,sz,k); }
    } else {
      int tt= ty==5? 2 : (ty==0||ty==2)? 0 : 1, arr= ty==2||ty==4;
      int ts=tex_slot2(c,off,tt,arr,0,1,bl,bank);
      if(ba){ c->uses_img_raw=1; c->si->sutex_mask|=1u<<ts; crd= ty==0? T("int(%s) / swr_tb[%d]",R(c,ra),ts) : ty==2||ty==3? T("ivec2(int(%s) / swr_tb[%d], int(%s))",R(c,ra),ts,R(c,ra+1)) : T("ivec3(int(%s) / swr_tb[%d], int(%s), int(%s))",R(c,ra),ts,R(c,ra+1),R(c,ra+2)); }
      if(tt==0) crd= ty==0? T("ivec2(%s, 0)",crd) : T("ivec3((%s).x, 0, (%s).y)",crd,crd);   /* 1D is bound as 2D (array) */
      const char* f=T("texelFetch(tex%d, %s, 0)",ts,crd);
      if(!isd){ emitl(c,"{ vec4 t_ = %s;",f); c->indent++; write_masked(c,rdd,B(w,20,4),"t_"); c->indent--; emitl(c,"}"); }
      else { c->uses_img_raw=1; c->si->sutex_mask|=1u<<ts;
        emitl(c,"{ uvec4 t_ = swr_v2r(%s, %uu, swr_tc[%d]);",f,sz,ts); c->indent++;
        for(u32 q=0;q<nw;q++) if(rdd!=255) setr(c,rdd+q,T("t_.%c","xyzw"[q])); c->indent--; emitl(c,"}"); }
    }
    break; }
  case O_TLDS: {
    u32 enc=B(w,53,4), ra=a, rb=B(w,20,8); u32 off=B(w,36,13);
    int type=1, array=0; const char* crd; const char* lod="0"; const char* ofs=NULL;
    switch(enc){
      case 0: type=0; crd=T("int(%s)",R(c,ra)); break;
      case 1: type=0; crd=T("int(%s)",R(c,ra)); lod=T("int(%s)",R(c,rb)); break;
      case 2: crd=T("ivec2(%s,%s)",R(c,ra),R(c,rb)); break;
      case 4: crd=T("ivec2(%s,%s)",R(c,ra),R(c,ra+1)); ofs=R(c,rb); break;
      case 5: crd=T("ivec2(%s,%s)",R(c,ra),R(c,ra+1)); lod=T("int(%s)",R(c,rb)); break;
      case 6: crd=T("ivec2(%s,%s)",R(c,ra),R(c,ra+1)); break;
      case 7: type=2; crd=T("ivec3(%s,%s,%s)",R(c,ra),R(c,ra+1),R(c,rb)); break;
      case 8: array=1; crd=T("ivec3(%s,%s,%s)",R(c,ra+1),R(c,rb),R(c,ra)); break;
      case 12: crd=T("ivec2(%s,%s)",R(c,ra),R(c,ra+1)); lod=T("int(%s)",R(c,rb)); ofs=R(c,rb+1); break;
      default: crd=T("ivec2(%s,%s)",R(c,ra),R(c,rb)); break;
    }
    int s=tex_slot(c,off,type,array,0,1);
    if(type==0) crd=T("ivec2(%s, 0)",crd);
    const char* smp= ofs? T("texelFetch(tex%d, %s + ivec2(bitfieldExtract(int(%s),0,4),bitfieldExtract(int(%s),4,4)), %s)",s,crd,ofs,ofs,lod) : T("texelFetch(tex%d, %s, %s)",s,crd,lod);
    emitl(c,"{ vec4 t_ = %s;",IW(c,s,smp)); c->indent++; write_texs(c,w,"t_",!B(w,59,1)); c->indent--; emitl(c,"}");   /* bit59: fp32 */
    break; }
  case O_TLD4S: {
    u32 ra=a, rb=B(w,20,8); u32 off=B(w,36,13); int dc=B(w,50,1), ao=B(w,51,1); u32 comp=B(w,52,2);
    const char* crd=T("vec2(%s,%s)",RF(c,ra),RF(c,rb)); const char* ofs=NULL; const char* dref=NULL;
    if(ao || dc){ crd=T("vec2(%s,%s)",RF(c,ra),RF(c,ra+1)); if(ao){ ofs=R(c,rb); if(dc) dref=RF(c,rb+1); } else dref=RF(c,rb); }
    int s=tex_slot(c,off,1,0,dc,0);
    const char* smp;
    if(dc) smp= ofs? T("textureGatherOffset(tex%d, %s, %s, ivec2(bitfieldExtract(int(%s),0,6),bitfieldExtract(int(%s),8,6)))",s,crd,dref,ofs,ofs) : T("textureGather(tex%d, %s, %s)",s,crd,dref);
    else smp= ofs? T("textureGatherOffset(tex%d, %s, ivec2(bitfieldExtract(int(%s),0,6),bitfieldExtract(int(%s),8,6)), %u)",s,crd,ofs,ofs,comp) : T("textureGather(tex%d, %s, %u)",s,crd,comp);
    emitl(c,"{ vec4 t_ = %s;",IW(c,s,smp)); c->indent++;
    { u32 da=d, db=B(w,28,8); if(B(w,55,1)){ setr(c,da,"packHalf2x16(t_.xy)"); setr(c,db,"packHalf2x16(t_.zw)"); }
      else { setrf(c,da,"t_.x"); if(da!=255) setrf(c,da+1,"t_.y"); if(db!=255){ setrf(c,db,"t_.z"); setrf(c,db+1,"t_.w"); } } }
    c->indent--; emitl(c,"}"); break; }
  case O_TEX: case O_TEX_B: case O_TLD4: case O_TLD: case O_TMML: case O_TXD: {
    int eid= id==O_TEX_B? O_TEX : id, bl= id==O_TEX_B; u32 blbank=0, bloff=0;
    u32 ty=B(w,28,3), mask=B(w,31,4), off=B(w,36,13); u32 cr=a, mr=B(w,20,8);
    if(bl){ if(!track_handle(c,idx,mr,&blbank,&bloff)){ unsupported(c,"TEX.B (untracked handle)",w); break; } off=bloff/4; mr++; }
    int type=(int)(ty>>1), array=ty&1; if(type>3) type=3;
    int dc=0, aoffi=0; u32 blod=0;
    if(eid==O_TEX){ dc=B(w,50,1); aoffi=B(w,54,1); blod=B(w,55,3); }
    else if(eid==O_TLD4){ dc=B(w,50,1); aoffi=B(w,54,2)!=0; }
    else if(eid==O_TLD){ blod= B(w,55,1)? 3 : 1; aoffi=B(w,35,1); }
    int dim= type==0? 1 : type==1? 2 : 3;
    const char* arr=NULL; u32 r=cr;
    if(array){ arr=R(c,r); r++; }
    const char* crd;
    if(eid==O_TLD){ crd= dim==1? T("int(%s)",R(c,r)) : dim==2? T("ivec2(%s,%s)",R(c,r),R(c,r+1)) : T("ivec3(%s,%s,%s)",R(c,r),R(c,r+1),R(c,r+2)); }
    else crd= dim==1? RF(c,r) : dim==2? T("vec2(%s,%s)",RF(c,r),RF(c,r+1)) : T("vec3(%s,%s,%s)",RF(c,r),RF(c,r+1),RF(c,r+2));
    u32 m=mr; const char* lod=NULL; const char* bias=NULL; const char* ofs=NULL; const char* dref=NULL;
    if(blod==2||blod==6){ bias=RF(c,m); m++; } else if(blod==3||blod==7){ lod= eid==O_TLD? T("int(%s)",R(c,m)) : RF(c,m); m++; } else if(blod==1){ lod= eid==O_TLD? "0" : "0.0"; }
    if(aoffi){ ofs=R(c,m); m++; }
    if(dc){ dref=RF(c,m); m++; }
    int s=tex_slot2(c,off,type,array,dc,eid==O_TLD,bl,blbank);
    if(type==0){ crd= eid==O_TLD? T("ivec2(%s, 0)",crd) : T("vec2(%s, 0.5)",crd); dim=2; }
    if(array && id!=O_TLD) crd=T("vec%d(%s, float(%s))",dim+1,crd,arr);
    if(array && eid==O_TLD) crd=T("ivec%d(%s, int(%s))",dim+1,crd,arr);
    const char* smp;
    if(eid==O_TMML){ smp= g_glsl_es? "vec4(0.0)" : T("vec4(textureQueryLod(tex%d, %s), 0.0, 0.0)",s,crd); }
    else if(eid==O_TLD){ smp=T("texelFetch(tex%d, %s, %s)",s,crd,lod?lod:"0"); }
    else if(eid==O_TLD4){ u32 comp=B(w,56,2); smp= dc? T("textureGather(tex%d, %s, %s)",s,crd,dref) : T("textureGather(tex%d, %s, %u)",s,crd,comp); }
    else if(eid==O_TXD){ smp=T("texture(tex%d, %s)",s,crd); }
    else {
      const char* cc=crd; int dc_comb= dc && !(dim==3 && array);
      if(dc) cc= dc_comb? T("vec%d(%s, %s)",dim+array+1,crd,dref) : crd;
      if(lod) smp= dc? T("vec4(textureLod(tex%d, %s, %s))",s,cc,lod) : T("textureLod(tex%d, %s, %s)",s,cc,lod);
      else if(bias) smp= dc? T("vec4(texture(tex%d, %s, %s))",s,cc,bias) : T("texture(tex%d, %s, %s)",s,cc,bias);
      else smp= dc? (dc_comb? T("vec4(texture(tex%d, %s))",s,cc) : T("vec4(texture(tex%d, %s, %s))",s,cc,dref)) : T("texture(tex%d, %s)",s,cc);
      /* offset from a register. GLSL requires a constant expression here; NVIDIA lets it slide, AMD/Mesa
         reject the shader. when the driver refuses (g_texoff_emul, see gl_selftest) add it to the coord
         in level 0 texels instead */
      /* texel offsets (AOFFI) with an explicit level or a bias too: the game's FXAA reads its neighbours with LZ +
         offset, and without them every neighbour read was the centre texel (FXAA did nothing on OpenGL). GLSL wants a
         constant offset; NVIDIA takes a register one, other drivers refuse it (g_texoff_emul): then it is added to the
         coordinates in texels of the level read */
      if(ofs && !dc && dim==2){
        extern int g_texoff_emul;
        const char* o=T("ivec2(bitfieldExtract(int(%s),0,4),bitfieldExtract(int(%s),4,4))",ofs,ofs);
        if(g_texoff_emul){ const char* lv= lod? T("int(%s)",lod) : "0";
          const char* moved= array? T("%s + vec3(vec2(%s)/vec2(textureSize(tex%d,%s).xy), 0.0)",cc,o,s,lv) : T("%s + vec2(%s)/vec2(textureSize(tex%d,%s))",cc,o,s,lv);
          smp= lod? T("textureLod(tex%d, %s, %s)",s,moved,lod) : bias? T("texture(tex%d, %s, %s)",s,moved,bias) : T("texture(tex%d, %s)",s,moved); }
        else smp= lod? T("textureLodOffset(tex%d, %s, %s, %s)",s,cc,lod,o) : bias? T("textureOffset(tex%d, %s, %s, %s)",s,cc,o,bias) : T("textureOffset(tex%d, %s, %s)",s,cc,o); }
    }
    emitl(c,"{ vec4 t_ = %s;",IW(c,s,smp)); c->indent++; write_masked(c,d,mask? mask : 15,"t_"); c->indent--; emitl(c,"}");
    break; }
  case O_TXQ: case O_TXQ_B: {
    u32 q=B(w,22,6), mask=B(w,31,4), off=B(w,36,13); int bl= id==O_TXQ_B; u32 blbank=0, bloff=0;
    if(bl){ if(!track_handle(c,idx,a,&blbank,&bloff)){ unsupported(c,"TXQ.B (untracked handle)",w); break; } off=bloff/4; a++; }
    int s=tex_slot2(c,off,1,0,0,1,bl,blbank);
    if(q==1){ emitl(c,"{ ivec2 sz_ = textureSize(tex%d, int(%s)); uvec4 t_ = uvec4(uint(sz_.x), uint(sz_.y), 1u, %s);",s,R(c,a),g_glsl_es? "1u" : T("uint(textureQueryLevels(tex%d))",s)); c->indent++;
      u32 r=d; for(int k=0;k<4;k++) if(mask&(1u<<k)){ setr(c,r,T("t_.%c","xyzw"[k])); r++; } c->indent--; emitl(c,"}"); }
    else { u32 r=d; for(int k=0;k<4;k++) if(mask&(1u<<k)){ setr(c,r,"0u"); r++; } }
    break; }
  /* ---------------- control flow */
  case O_BRX: { i32 off=(i32)(B(w,20,24)<<8)>>8;
    emitl(c,"{ uint t_ = uint(%d + (int(%s) + (%d))/8); if(t_>=10u && ((t_-10u)&3u)==0u) t_++; pc = t_; break; }",(int)idx+1,R(c,a),off); break; }
  case O_BRA: { u32 t=bra_target(w,idx); if(B(w,0,5)!=15){ c->uses_cc=1; } emitl(c,"pc = %uu; break;",t); break; }
  /* if the stacks were resolved statically: push = nothing, pop = fixed jump */
  case O_SSY: if(!c->stk_static){ c->uses_stk[0]=1; emitl(c,"ssy[ssy_sp++] = %uu;",bra_target(w,idx)); } break;
  case O_PBK: if(!c->stk_static){ c->uses_stk[1]=1; emitl(c,"pbk[pbk_sp++] = %uu;",bra_target(w,idx)); } break;
  case O_PCNT: if(!c->stk_static){ c->uses_stk[2]=1; emitl(c,"pcnt[pcnt_sp++] = %uu;",bra_target(w,idx)); } break;
  case O_SYNC: if(c->stk_static) emitl(c,"pc = %uu; break;",c->pop_tgt[idx]); else { c->uses_stk[0]=1; emitl(c,"pc = ssy[--ssy_sp]; break;"); } break;
  case O_BRK: if(c->stk_static) emitl(c,"pc = %uu; break;",c->pop_tgt[idx]); else { c->uses_stk[1]=1; emitl(c,"pc = pbk[--pbk_sp]; break;"); } break;
  case O_CONT: if(c->stk_static) emitl(c,"pc = %uu; break;",c->pop_tgt[idx]); else { c->uses_stk[2]=1; emitl(c,"pc = pcnt[--pcnt_sp]; break;"); } break;
  case O_EXIT: exit_epilogue(c); break;
  /* PRET/RET without CAL: RET goes to the PRET target = end of program (early exit in compute) */
  case O_PRET: break;
  case O_RET: exit_epilogue(c); break;
  case O_KIL: emitl(c,"discard;"); break;
  case O_NOP: case O_VOTE_VTG: case O_DEPBAR: case O_MEMBAR: case O_CCTL: case O_CCTLL: case O_IDE: break;
  case O_BAR: if(c->stage==ST_CS) emitl(c,"barrier();"); break;
  case O_VOTE: { const char* pr=PN(c,B(w,39,3),B(w,42,1)); u32 vop=B(w,48,2);
    setp(c,B(w,45,3), vop==2? "true" : pr); setr(c,d,T("(%s ? 1u : 0u)",pr)); break; }
  case O_SHFL: setr(c,d,R(c,a)); setp(c,B(w,48,3),"true"); break;
  case O_FSWZADD: {
    /* derivatives the nouveau way: SHFL.BFLY + FSWZADD.
       0x99 = QUADOP(SUB,SUBR,SUB,SUBR) -> dFdx(Rb), 0xA5 = QUADOP(SUB,SUB,SUBR,SUBR) -> dFdy(Rb) */
    u32 q=B(w,28,8); u32 rb=B(w,20,8);
    if(c->stage==ST_FS && q==0x99) setrf(c,d,T("dFdx(%s)",RF(c,rb)));
    else if(c->stage==ST_FS && q==0xA5) setrf(c,d,T("dFdy(%s)",RF(c,rb)));
    else if(c->stage==ST_FS && q==0x66) setrf(c,d,T("(-dFdx(%s))",RF(c,rb)));
    else if(c->stage==ST_FS && q==0x5A) setrf(c,d,T("(-dFdy(%s))",RF(c,rb)));
    else setrf(c,d,T("(%s + %s)",RF(c,a),RF(c,rb)));
    break; }
  case O_OUT_R: case O_OUT_C: case O_OUT_I: {
    u32 k=B(w,39,2); if(k&1) emitl(c,"EmitVertex();"); if(k&2) emitl(c,"EndPrimitive();"); setr(c,d,R(c,a)); break; }
  case O_HADD2_R: case O_HMUL2_R: case O_HFMA2_R: case O_HADD2_C: case O_HMUL2_C: case O_HADD2_I: case O_HMUL2_I: case O_HADD2_32I: case O_HMUL2_32I: case O_HFMA2_32I: case O_HFMA2_I: case O_HFMA2_RC: case O_HFMA2_CR: {
    /* packed f16 (same approach as yuzu): swizzle, neg/abs, sat, merge */
    int isadd= id==O_HADD2_R||id==O_HADD2_C||id==O_HADD2_I||id==O_HADD2_32I; int isfma= id==O_HFMA2_R||id==O_HFMA2_32I||id==O_HFMA2_I||id==O_HFMA2_RC||id==O_HFMA2_CR;
    int is32= id==O_HADD2_32I||id==O_HMUL2_32I||id==O_HFMA2_32I;
    u32 merge= is32? 0 : B(w,49,2), swa= B(w,47,2); int nega=0, absa=0, negb=0, absb=0, negc=0, sat=0; u32 swb=0, swc=0;
    const char* sb=NULL; const char* sc=NULL; int bimm=0;
    if(isadd){ nega=B(w,43,1); absa=B(w,44,1);
      if(id==O_HADD2_R){ negb=B(w,31,1); absb=B(w,30,1); swb=B(w,28,2); sb=R(c,B(w,20,8)); sat=B(w,32,1); }
      else if(id==O_HADD2_C){ negb=B(w,56,1); absb=B(w,54,1); swb=1; sb=CB(c,cb_idx(w),cb_off(w)); sat=B(w,52,1); }
      else if(id==O_HADD2_I){ bimm=1; sat=B(w,52,1); }
      else { nega=B(w,56,1); absa=0; swa=B(w,53,2); bimm=2; sat=B(w,52,1); } }
    else if(!isfma){ absa=B(w,44,1);
      if(id==O_HMUL2_R){ negb=B(w,31,1); absb=B(w,30,1); swb=B(w,28,2); sb=R(c,B(w,20,8)); sat=B(w,32,1); }
      else if(id==O_HMUL2_C){ nega=B(w,43,1); absb=B(w,54,1); swb=1; sb=CB(c,cb_idx(w),cb_off(w)); sat=B(w,52,1); }
      else if(id==O_HMUL2_I){ nega=B(w,43,1); bimm=1; sat=B(w,52,1); }
      else { absa=0; swa=B(w,53,2); bimm=2; sat=B(w,52,1); } }
    else {
      if(id==O_HFMA2_R){ negb=B(w,31,1); negc=B(w,30,1); swb=B(w,28,2); swc=B(w,35,2); sb=R(c,B(w,20,8)); sc=R(c,B(w,39,8)); sat=B(w,32,1); }
      else if(id==O_HFMA2_RC){ negb=B(w,56,1); negc=B(w,51,1); swb=B(w,53,2); swc=1; sb=R(c,B(w,39,8)); sc=CB(c,cb_idx(w),cb_off(w)); sat=B(w,52,1); }
      else if(id==O_HFMA2_CR){ negb=B(w,56,1); negc=B(w,51,1); swb=1; swc=B(w,53,2); sb=CB(c,cb_idx(w),cb_off(w)); sc=R(c,B(w,39,8)); sat=B(w,52,1); }
      else if(id==O_HFMA2_I){ negc=B(w,51,1); swc=B(w,53,2); sc=R(c,B(w,39,8)); bimm=1; sat=B(w,52,1); }
      else { negc=B(w,52,1); swa=B(w,53,2); swc=0; sc=R(c,d); bimm=2; } }
    if(bimm==1){ u32 lo=((B(w,20,9)<<6)|(B(w,29,1)<<15)), hi=((B(w,30,9)<<6)|(B(w,56,1)<<15)); sb=T("0x%Xu",lo|(hi<<16)); swb=0; }
    else if(bimm==2){ sb=imm32(w); swb=0; }
    static const char* swz[4]={"unpackHalf2x16(%s)","vec2(uintBitsToFloat(%s))","unpackHalf2x16(%s).xx","unpackHalf2x16(%s).yy"};
    emitl(c,"{");
    emitl(c,"vec2 ha=%s;",T(swz[swa&3],R(c,a))); if(absa) emitl(c,"ha=abs(ha);"); if(nega) emitl(c,"ha=-ha;");
    emitl(c,"vec2 hb=%s;",T(swz[swb&3],sb)); if(absb) emitl(c,"hb=abs(hb);"); if(negb) emitl(c,"hb=-hb;");
    if(isfma){ emitl(c,"vec2 hc=%s;",T(swz[swc&3],sc)); if(negc) emitl(c,"hc=-hc;"); }
    emitl(c,"vec2 hr=%s;", isadd? "ha+hb" : isfma? "ha*hb+hc" : "ha*hb");
    if(sat) emitl(c,"hr=clamp(hr,0.0,1.0);");
    if(merge==1) setr(c,d,"floatBitsToUint(hr.x)");
    else if(merge==2) setr(c,d,T("((%s & 0xFFFF0000u) | (packHalf2x16(hr) & 0xFFFFu))",R(c,d)));
    else if(merge==3) setr(c,d,T("((%s & 0xFFFFu) | (packHalf2x16(hr) & 0xFFFF0000u))",R(c,d)));
    else setr(c,d,"packHalf2x16(hr)");
    emitl(c,"}"); break; }
  case O_HSETP2_R: case O_HSETP2_C: case O_HSETP2_I: {
    /* packed f16 compare; h_and combines both halves */
    static const char* swz[4]={"unpackHalf2x16(%s)","vec2(uintBitsToFloat(%s))","unpackHalf2x16(%s).xx","unpackHalf2x16(%s).yy"};
    u32 cmp, hand, swb; int negb, absb; const char* sb;
    if(id==O_HSETP2_R){ cmp=B(w,35,4); hand=B(w,49,1); negb=B(w,31,1); absb=B(w,30,1); swb=B(w,28,2); sb=R(c,B(w,20,8)); }
    else if(id==O_HSETP2_C){ cmp=B(w,49,4); hand=B(w,53,1); negb=B(w,56,1); absb=B(w,54,1); swb=1; sb=CB(c,cb_idx(w),cb_off(w)); }
    else { cmp=B(w,49,4); hand=B(w,53,1); negb=0; absb=0; swb=0;
      u32 lo=((B(w,20,9)<<6)|(B(w,29,1)<<15)), hi=((B(w,30,9)<<6)|(B(w,56,1)<<15)); sb=T("0x%Xu",lo|(hi<<16)); }
    const char* pc=PN(c,B(w,39,3),B(w,42,1)); u32 pa=B(w,3,3), pb=B(w,0,3), bo=B(w,45,2);
    emitl(c,"{");
    emitl(c,"vec2 ha=%s;",T(swz[B(w,47,2)],R(c,a))); if(B(w,44,1)) emitl(c,"ha=abs(ha);"); if(B(w,43,1)) emitl(c,"ha=-ha;");
    emitl(c,"vec2 hb=%s;",T(swz[swb&3],sb)); if(absb) emitl(c,"hb=abs(hb);"); if(negb) emitl(c,"hb=-hb;");
    const char* cx=fcmp_expr(cmp,"ha.x","hb.x"); const char* cy=fcmp_expr(cmp,"ha.y","hb.y");
    if(hand) setp2(c,pa,pb,bo,T("(%s && %s)",cx,cy),pc);
    else { setp(c,pa,bop(bo,cx,pc)); setp(c,pb,bop(bo,cy,pc)); }
    emitl(c,"}"); break; }
  default:
    unsupported(c,op->name,w); break;
  }
}

/* ============================================================ walk */
static int is_term(int id){ return id==O_BRA||id==O_BRX||id==O_SYNC||id==O_BRK||id==O_CONT||id==O_EXIT||id==O_RET||id==O_JMP; }

static void mark(Ctx* c, u32 i){ if(i<65536) c->leader[i>>3]|=1<<(i&7); }

static int is_leader(Ctx* c, u32 i){ return i<65536 && (c->leader[i>>3]>>(i&7))&1; }

static void mark_tgt(Ctx* c, u32 i){ mark(c,i); if(i<65536) c->tgt[i>>3]|=1<<(i&7); }

static int is_tgt(Ctx* c, u32 i){ return i<65536 && (c->tgt[i>>3]>>(i&7))&1; }

int (*g_shader_cb_read)(u32 idx, u32 off, u32* out);

u64 shader_hash(const u8* code, u32 max_bytes, u32* out_len){
  if(!ops_ready) ops_init();
  const u64* w=(const u64*)code; u32 n=max_bytes/8; u32 i=11, last=11;
  /* linear scan until an unconditional EXIT past every branch target */
  u32 maxt=0;
  for(;i<n;i++){
    if(!IDX_OK(i)) continue;
    const Op* op=decode(w[i]); last=i;
    if(!op) continue;
    if(op->id==O_BRA||op->id==O_SSY||op->id==O_PBK||op->id==O_PCNT){ u32 t=bra_target(w[i],i); if(t>maxt) maxt=t; }
    if(op->id==O_EXIT && B(w[i],16,4)==7 && i>=maxt) break;
    if(op->id==O_BRA && B(w[i],16,4)==7 && B(w[i],0,5)==15 && i>=maxt && bra_target(w[i],i)<=i) break;
    if(i>16384) break;
  }
  u32 len=(last+1)*8; if(len>max_bytes) len=max_bytes;
  if(out_len) *out_len=len;
  u64 h=1469598103934665603ull; for(u32 k=0;k<len;k++){ h^=code[k]; h*=1099511628211ull; }
  return h;
}

/* a Maxwell program image (SPH + code) found in a file (shader pre-build, gpu_gl.c): 1 if its header is a vertex or
   pixel program's, it ends the way shader_hash ends a real program (EXIT or a self-branch past every branch target),
   and at least 95% of its instructions decode. *out_len is the length shader_hash would hash, *type the SPH type */
int shader_program_check(const u8* code, u32 max_bytes, u32* out_len, int* type){
  if(!ops_ready) ops_init();
  if(max_bytes<16*8) return 0;
  u32 w0; memcpy(&w0,code,4); u32 sph=w0&31, ver=(w0>>5)&31, ty=(w0>>10)&15;
  if(ver!=3 || !((sph==1 && ty>=1 && ty<=4) || (sph==2 && ty==5))) return 0;
  const u64* w=(const u64*)code; u32 n=max_bytes/8, maxt=0, tot=0, bad=0;
  for(u32 i=11;i<n && i<=16384;i++){
    if(!IDX_OK(i)) continue;
    const Op* op=decode(w[i]); tot++;
    if(!op){ bad++; continue; }
    if(op->id==O_BRA||op->id==O_SSY||op->id==O_PBK||op->id==O_PCNT){ u32 t=bra_target(w[i],i); if(t>maxt) maxt=t; }
    if((op->id==O_EXIT && B(w[i],16,4)==7 && i>=maxt) ||
       (op->id==O_BRA && B(w[i],16,4)==7 && B(w[i],0,5)==15 && i>=maxt && bra_target(w[i],i)<=i)){
      if(out_len) *out_len=(i+1)*8; if(type) *type=(int)ty;
      return tot>=2 && bad*20<=tot; }
  }
  return 0;
}

/* BRX targets from its jump table (LDC nearby -> cbuf). -1 if we can't tell */
static int brx_targets(const u64* w, u32 i, u32 n, u32* out, int max){
  if(!g_shader_cb_read) return -1;
  u32 ra=B(w[i],8,8); i32 imm=(i32)(B(w[i],20,24)<<8)>>8;
  for(u32 k=1;k<=8 && k<i;k++){ u32 j=i-k; if(!IDX_OK(j)) continue; const Op* o2=decode(w[j]);
    if(!o2 || o2->id!=O_LDC || B(w[j],0,8)!=ra) continue;
    u32 cidx=B(w[j],36,5); i32 coff=(i32)(B(w[j],20,16)<<16)>>16; int cnt=0, seen=0;
    for(u32 e=0;e<64 && cnt<max;e++){ u32 v; if(!g_shader_cb_read(cidx,(u32)(coff+(i32)e*4),&v)) break;
      i32 bb=(i32)v+imm; if(bb<0 || (bb&7)) break; u32 t=i+1+(u32)(bb/8); if(t>=10 && ((t-10)&3)==0) t++;
      if(t<11) break; seen++; if(t>=n) continue; out[cnt++]=t; }
    return seen? cnt : -1; }
  return -1; }
/* ---- static SSY/PBK/PCNT resolution ----
   Walk the CFG carrying the 3 stacks. If every reachable block sees the same stacks on all incoming paths,
   each SYNC/BRK/CONT has a fixed target and we can drop the stacks. Any conflict (or underflow, or depth > 16)
   falls back to the dynamic version. */
typedef struct { u8 n[3]; u32 s[3][16]; } Stk;

static int stk_eq(const Stk* a, const Stk* b){ for(int k=0;k<3;k++){ if(a->n[k]!=b->n[k] || memcmp(a->s[k],b->s[k],a->n[k]*4)) return 0; } return 1; }
typedef struct { Ctx* c; u32 n; int fwd; int* lid; Stk* st; u8* has; int* wl; int nwl; } StkA;

/* edge i -> tg, 0 on conflict */
static int stk_succ(StkA* A, u32 i, u32 tg, const Stk* s){
  if(A->fwd && tg<=i) return 1;                               /* linear mode: no later block -> return */
  if(tg>=A->n || !is_leader(A->c,tg)) return 1;               /* no block -> return */
  int L=A->lid[tg]; if(A->has[L]) return stk_eq(&A->st[L],s);
  A->st[L]=*s; A->has[L]=1; A->wl[A->nwl++]=L; return 1; }

static int stk_resolve(Ctx* c, u32 n, int fwd, const u32* brx_all, int brx_n){
  int nl=0; for(u32 i=11;i<n;i++) if(IDX_OK(i) && is_leader(c,i)) nl++;
  StkA A={c,n,fwd}; A.lid=malloc(sizeof(int)*(n+1)); u32* ldr=malloc(sizeof(u32)*(nl+1)); A.st=malloc(sizeof(Stk)*(nl+1)); A.has=calloc(nl+1,1); A.wl=malloc(sizeof(int)*(nl+1));
  int k=0; for(u32 i=11;i<n;i++) if(IDX_OK(i) && is_leader(c,i)){ A.lid[i]=k; ldr[k++]=i; }
  c->pop_tgt=calloc(n+1,sizeof(u32));
  int ok=1; Stk s0; memset(&s0,0,sizeof s0); A.st[0]=s0; A.has[0]=1; A.wl[A.nwl++]=0;
  while(ok && A.nwl){ int L=A.wl[--A.nwl]; Stk s=A.st[L];
    for(u32 i=ldr[L];i<n && ok;i++){
      if(!IDX_OK(i)) continue;
      if(i!=ldr[L] && is_leader(c,i)){ ok=stk_succ(&A,i-1,i,&s); break; }   /* fall through */
      const Op* op=decode(c->w[i]); if(!op) continue;
      u64 w=c->w[i]; int id=op->id; u32 pg=B(w,16,3), pn=B(w,19,1); int always= pg==7 && !pn;
      int kk= id==O_SSY||id==O_SYNC? 0 : id==O_PBK||id==O_BRK? 1 : 2;
      if(id==O_SSY||id==O_PBK||id==O_PCNT){ if(s.n[kk]>=16){ ok=0; break; } s.s[kk][s.n[kk]++]=bra_target(w,i); continue; }   /* unpredicated */
      if(pg==7 && pn) continue;                                           /* never runs */
      if(id==O_SYNC||id==O_BRK||id==O_CONT){ if(!s.n[kk]){ ok=0; break; } u32 tg=s.s[kk][s.n[kk]-1];
        if(c->pop_tgt[i] && c->pop_tgt[i]!=tg){ ok=0; break; } c->pop_tgt[i]=tg;
        Stk s2=s; s2.n[kk]--; ok=stk_succ(&A,i,tg,&s2); if(always) break; continue; }
      if(id==O_BRA){ ok=stk_succ(&A,i,bra_target(w,i),&s); if(always) break; continue; }
      if(id==O_BRX){ for(int q=0;q<brx_n && ok;q++) ok=stk_succ(&A,i,brx_all[q],&s); if(always) break; continue; }
      if(id==O_EXIT && always) break;
    } }
  free(A.lid); free(ldr); free(A.st); free(A.has); free(A.wl);
  return ok; }

/* ---- structured emission for forward-only shaders (nested if/else, no pc var) ----
   forward branch i->t (BRA, or SYNC/BRK once resolved) opens 'if(!cond){' and closes at t. needs properly
   nested ranges; an unconditional branch right before a close turns into 'else'. otherwise use the pc guards. */
typedef struct { u32 end, end2; u8 els, in_else; } Iv;
#define ST_MAX 64

/* target of branch i, 0 if not a branch, ~0u if it breaks structuring */
static u32 st_jump(Ctx* c, u32 i, const Op* op){
  u64 w=c->w[i]; int id=op->id;
  if(id!=O_BRA && id!=O_SYNC && id!=O_BRK) return 0;
  if(B(w,16,3)==7 && B(w,19,1)) return 0;                   /* never runs */
  u32 t= id==O_BRA? bra_target(w,i) : c->pop_tgt[i];
  if(t<=i) return ~0u;                                       /* backward branch or unreachable pop */
  u32 nx=i+1; while(nx<c->nwords && !IDX_OK(nx)) nx++;
  return t==nx? 0 : t; }                                     /* branch to i+1, no-op */
static int st_uncond(Ctx* c, u32 i){ u64 w=c->w[i]; return B(w,16,3)==7 && !B(w,19,1); }

/* jt[i]: 0 normal, ~0u becomes 'else', otherwise the equivalent nearer target */
static int st_check(Ctx* c, u32 n, u32* jt){
  Iv S[ST_MAX]; int sp=0;
  for(u32 i=11;i<n;i++){ if(!IDX_OK(i)) continue;
    while(sp && S[sp-1].end==i){ if(S[sp-1].els && !S[sp-1].in_else){ S[sp-1].in_else=1; S[sp-1].end=S[sp-1].end2; } else sp--; }
    const Op* op=decode(c->w[i]); if(!op) continue;
    u32 t=st_jump(c,i,op); if(!t) continue; if(t==~0u) return 0;
    if(sp && t>S[sp-1].end){   /* jumps into the current if */
      Iv* tp=&S[sp-1]; u32 e=tp->end, g=e-1; while(g>i && !IDX_OK(g)) g--;
      /* then-block ending in a jump to the same target: equivalent to targeting that jump */
      const Op* og= g>i && g<n? decode(c->w[g]) : NULL;
      if(og && st_uncond(c,g) && st_jump(c,g,og)==t){ jt[i]=g; t=g; }
      else { /* uncond branch right before the close -> rest up to t is the else */
        u32 nx=i+1; while(nx<n && !IDX_OK(nx)) nx++;
        if(st_uncond(c,i) && nx==e && e<n && !tp->els && (sp<2 || (S[sp-2].end>e && t<=S[sp-2].end))){ tp->els=1; tp->end2=t; jt[i]=~0u; continue; }
        return 0; } }
    if(sp>=ST_MAX) return 0;
    S[sp].end=t; S[sp].end2=0; S[sp].els=0; S[sp].in_else=0; sp++; }
  return 1; }

char* shader_to_glsl(const u8* code, u32 max_bytes, int stage, ShaderInfo* si){
  if(!ops_ready) ops_init();
  Ctx* c=calloc(1,sizeof(Ctx)); c->w=(const u64*)code; c->stage=stage; c->si=si;
  u32 attr_int=si->attr_int_mask, attr_sint=si->attr_sint_mask, flat=si->fs_flat_mask; u32 ktm=si->tex_int_mask, kts=si->tex_sint_mask, krm=si->rt_int_mask, krs=si->rt_sint_mask;
  u32 kcl[3]={si->cs_local[0],si->cs_local[1],si->cs_local[2]}; int ksh=si->shared_size;
  /* keep attr_sint_mask too: signed integer attributes must be declared ivec4, not uvec4 */
  memset(si,0,sizeof *si); si->stage=stage; memcpy(si->cs_local,kcl,sizeof kcl); si->shared_size=ksh; si->attr_int_mask=attr_int; si->attr_sint_mask=attr_sint; si->fs_flat_mask=flat; si->tex_int_mask=ktm; si->tex_sint_mask=kts; si->rt_int_mask=krm; si->rt_sint_mask=krs;
  memcpy(si->sph,code,80);
  u32 len; shader_hash(code,max_bytes,&len); u32 n=len/8; c->nwords=n;
  /* leaders + branch targets */
  mark(c,11); int flow=0, has_brx=0, all_ldr=0, has_stk=0;
  for(u32 i=11;i<n;i++){ if(!IDX_OK(i)) continue; const Op* op=decode(c->w[i]); if(!op) continue;
    int id=op->id;
    if(id==O_BRA||id==O_SSY||id==O_PBK||id==O_PCNT){ mark_tgt(c,bra_target(c->w[i],i)); flow=1; }
    if(id==O_SSY||id==O_PBK||id==O_PCNT) has_stk=1;
    if(id==O_BRX){ u32 tg[64]; int nt=brx_targets(c->w,i,n,tg,64); has_brx=1;  /* with a known table only its targets go in the switch */
      if(nt>=0){ for(int q=0;q<nt;q++) mark_tgt(c,tg[q]); } else { all_ldr=1; for(u32 k=11;k<n;k++) if(IDX_OK(k)) mark_tgt(c,k); } flow=1; }
    if(is_term(id) || id==O_BRA){ u32 nx=i+1; while(nx<n && !IDX_OK(nx)) nx++; mark(c,nx); if(id!=O_EXIT) flow=1; }
  }
  c->has_flow=flow;
  /* all branches forward (no loops): skip the switch loop and emit blocks in order behind pc guards.
     compiles and runs a lot faster. a BRX with a forward-only table also works this way */
  int fwd=flow && !getenv("SWR_SHADER_SWITCH");
  for(u32 i=11;i<n && fwd;i++){ if(!IDX_OK(i)) continue; const Op* op=decode(c->w[i]); if(!op) continue; int id=op->id;
    if(id==O_BRX){ u32 tg[64]; int nt=brx_targets(c->w,i,n,tg,64); if(nt<0) fwd=0; for(int q=0;q<nt;q++) if(tg[q]<=i) fwd=0; }
    else if(id==O_PCNT||id==O_CONT||id==O_CAL||id==O_RET||id==O_JMP) fwd=0;
    else if((id==O_BRA||id==O_SSY||id==O_PBK) && bra_target(c->w[i],i)<=i) fwd=0; }
  /* resolve the stacks; a BRX can reach any block so all of them count as successors */
  if(flow && has_stk && !all_ldr && !getenv("SWR_SHADER_DYNSTACK")){
    u32* bl=NULL; int nb=0; if(has_brx){ bl=malloc(sizeof(u32)*(n+1)); for(u32 i=11;i<n;i++) if(IDX_OK(i) && is_leader(c,i)) bl[nb++]=i; }
    c->stk_static=stk_resolve(c,n,fwd,bl,nb); free(bl); }
  /* linear, no BRX: only branch targets need a guard */
  int merge= fwd && !has_brx;
  u32* jt=NULL; int stru=0;
  if(fwd && !has_brx && (!has_stk || c->stk_static) && !getenv("SWR_SHADER_NOSTRUCT")){ jt=calloc(n+1,sizeof(u32)); stru=st_check(c,n,jt); }
  /* body */
  c->indent= flow? 3 : 1;
  size_t exit_end=(size_t)-1;   /* where the last top level EXIT's text ends */
  if(stru){ Iv S[ST_MAX]; int sp=0;
    for(u32 i=11;i<n;i++){
      if(!IDX_OK(i)) continue;
      while(sp && S[sp-1].end==i){ if(S[sp-1].els && !S[sp-1].in_else){ c->indent=sp; emitl(c,"} else {"); S[sp-1].in_else=1; S[sp-1].end=S[sp-1].end2; }
        else { sp--; c->indent=sp+1; emitl(c,"}"); } }
      c->indent=sp+1;
      c->nest=sp;   /* forward only, so writes in any branch already clear labels as we go */
      const Op* op=decode(c->w[i]);
      if(!op){ c->si->unsupported++; emitl(c,"/* desconocida %016llx */",(unsigned long long)c->w[i]); continue; }
      u32 t=st_jump(c,i,op);
      if(t){ if(jt[i]!=~0u){ u64 w=c->w[i]; u32 pg=B(w,16,3); if(jt[i]) t=jt[i];
          emitl(c,"if(%s) {", pg==7? "false" : B(w,19,1)? P(c,pg) : T("(!%s)",P(c,pg)));
          S[sp].end=t; S[sp].end2=0; S[sp].els=0; S[sp].in_else=0; sp++; }
        else { S[sp-1].els=1; S[sp-1].end2=t; }   /* branch ending the then -> else */
        continue; }
      if(op->id==O_BRA||op->id==O_SYNC||op->id==O_BRK) continue;   /* never taken / goes to i+1 */
      emit_tracked(c,i,c->w[i],op);
      if(sp==0 && op->id==O_EXIT && st_uncond(c,i)) exit_end=c->out.n;
    }
    while(sp){ sp--; c->indent=sp+1; emitl(c,"}"); }
    if(exit_end!=c->out.n){ c->indent=1; emitl(c,"return;"); } }   /* no second return after the final EXIT */
  else if(fwd) sb_put(&c->out,"  uint pc = 11u;\n");
  else if(flow){ sb_put(&c->out,"  uint pc = 11u; uint guard_ = 0u;\n  while(true){\n    if(++guard_ > 16384u) return;\n    switch(pc){\n"); }
  /* dead code still goes through translation (keeps ShaderInfo/decls the same), the text is thrown away */
  int dead=0; Sb live={0};
  for(u32 i=11;i<n && !stru;i++){
    if(!IDX_OK(i)) continue;
    int opens= flow && is_leader(c,i) && (i==11 || !merge || is_tgt(c,i));
    if(opens && dead){ free(c->out.p); c->out=live; dead=0; }
    else if(opens && fwd && i!=11){ c->indent=3; emitl(c,"pc = %uu; break;",i); }
    if(opens && fwd){ if(i!=11){ c->indent=2; emitl(c,"} while(false);"); } c->indent=2; emitl(c,"if(pc == %uu) do {",i); c->indent=3; }
    else if(opens){ c->indent=2; emitl(c,"case %uu:",i); c->indent=3; }   /* no 'pc = i; break;', fall through */
    if(is_leader(c,i)) memset(c->wtag,0,sizeof c->wtag);
    const Op* op=decode(c->w[i]);
    if(!op){ c->si->unsupported++; emitl(c,"/* desconocida %016llx */",(unsigned long long)c->w[i]); continue; }
    emit_tracked(c,i,c->w[i],op);
    int id=op->id; u64 w=c->w[i];
    if(!flow && id==O_EXIT && B(w,16,3)==7 && !B(w,19,1)) exit_end=c->out.n;
    if(flow && !dead && B(w,16,3)==7 && !B(w,19,1) && (id==O_BRA||id==O_BRX||id==O_SYNC||id==O_BRK||id==O_CONT||id==O_EXIT)){
      dead=1; live=c->out; memset(&c->out,0,sizeof c->out); }
  }
  if(dead){ free(c->out.p); c->out=live; }
  free(jt);
  if(stru){}
  else if(fwd){ if(!dead){ c->indent=3; emitl(c,"return;"); } c->indent=2; emitl(c,"} while(false);"); c->indent=1; emitl(c,"return;"); }
  else if(flow){ if(!dead){ c->indent=3; emitl(c,"return;"); } sb_put(&c->out,"      default: return;\n    }\n  }\n"); }
  else if(exit_end!=c->out.n){ c->indent=1; exit_epilogue(c); }   /* dead if we already ended with EXIT */
  /* declarations */
  Sb* D=&c->decl;
  if(g_glsl_es){ sb_put(D,"#version 320 es\nprecision highp float;\nprecision highp int;\n");
    static const char* ps[]={"sampler2D","sampler3D","samplerCube","sampler2DArray","samplerCubeArray","sampler2DShadow","sampler2DArrayShadow","samplerCubeShadow","samplerCubeArrayShadow",
      "isampler2D","usampler2D","isampler3D","usampler3D","isampler2DArray","usampler2DArray"};
    for(unsigned k=0;k<sizeof ps/sizeof ps[0];k++) sb_f(D,"precision highp %s;\n",ps[k]); }
  else sb_f(D,"#version %d core\n",g_glsl_version);
  /* gl_Layer from the VS needs an extension on desktop GL; without one we render to layer 0 */
  if(stage!=ST_FS && si->writes_layer){
    if(g_vs_layer_ext==1) sb_put(D,"#extension GL_ARB_shader_viewport_layer_array : require\n#define SWR_SET_LAYER(v) gl_Layer=(v)\n");
    else if(g_vs_layer_ext==2) sb_put(D,"#extension GL_AMD_vertex_shader_layer : require\n#define SWR_SET_LAYER(v) gl_Layer=(v)\n");
    else if(g_vs_layer_ext==3) sb_put(D,"#extension GL_NV_viewport_array2 : require\n#define SWR_SET_LAYER(v) gl_Layer=(v)\n");
    else sb_put(D,"#define SWR_SET_LAYER(v)\n"); }
  if(stage==ST_FS && (si->sph[0]>>15&1)) {}
  if(stage==ST_FS && si->uses_position_in) sb_f(D,"layout(std140, binding=%d) uniform SWR_RS { vec4 swr_rs; };\n",g_rs_binding);   /* 1/res scale */
  /* packed binding numbers, Intel's windows driver has very few: VS 0.., FS 1.., others 2.. */
  int slot= stage==ST_VS? 0 : stage==ST_FS? 1 : 2;
  int base_bind=slot*18;
  for(int i=0;i<si->ngbuf;i++) sb_f(D,"layout(std430, binding=%d) %sbuffer S%d_G%d { uint d[]; } g%d;\n",stage==ST_CS? i : slot*8+i,((si->gbuf_wmask>>i)&1)? "" : "readonly ",stage,i,i);
  if(stage==ST_CS) sb_f(D,"layout(local_size_x=%u, local_size_y=%u, local_size_z=%u) in;\n",si->cs_local[0]? si->cs_local[0] : 1,si->cs_local[1]? si->cs_local[1] : 1,si->cs_local[2]? si->cs_local[2] : 1);
  for(int i=0;i<18;i++) if(si->cbuf_mask&(1u<<i)) sb_f(D,"layout(std140, binding=%d) uniform S%d_CB%d { vec4 d[4096]; } cb%d;\n",base_bind+i,stage,i,i);
  for(int i=0;i<si->ntex;i++){ ShTex* t=&si->tex[i]; sb_f(D,"layout(binding=%d) uniform %s S%d_tex%d;\n#define tex%d S%d_tex%d\n",slot*(g_glsl_es? 16 : 32)+i,T("%s%s",(!t->shadow && ((si->tex_int_mask>>i)&1))? (((si->tex_sint_mask>>i)&1)? "i" : "u") : "",samp_type(t->type,t->array,t->shadow)),stage,i,i,stage,i); }   /* stage-unique name */
  if(stage==ST_VS){
    for(int i=0;i<32;i++) if(c->in_used[i]){ si->in_mask|=1u<<i; int isint=(attr_int>>i)&1; sb_f(D,"layout(location=%d) in %s in_attr%d;\n",i,isint? ((si->attr_sint_mask>>i)&1? "ivec4":"uvec4") : "vec4",i); }
    for(int i=0;i<32;i++) if(c->out_used[i]){ si->out_mask|=1u<<i; sb_f(D,"layout(location=%d) out vec4 out_attr%d;\n",i,i); }
    /* invariant so depth matches across passes with different shaders */
    if(!g_glsl_es){ if(si->writes_layer) sb_put(D,"invariant gl_Position;\nout float gl_ClipDistance[8];\n");   /* Mesa drops gl_Layer when the block is redeclared */
      else sb_put(D,"out gl_PerVertex { invariant vec4 gl_Position; float gl_PointSize; float gl_ClipDistance[8]; };\n"); }
    else sb_put(D,"invariant gl_Position;\n");
  } else if(stage==ST_FS){
    const u8* imap=(const u8*)si->sph+0x18;
    for(int i=0;i<32;i++) if(c->in_used[i]){ si->in_mask|=1u<<i; u8 m=imap[i]; int f=(m&3)==1 || ((m>>2)&3)==1 || (flat>>i)&1; int lin=(m&3)==3;
      sb_f(D,"layout(location=%d) %sin vec4 in_attr%d;\n",i,f? "flat " : (lin && !g_glsl_es)? "noperspective " : "",i); }
    u32 target=si->sph[18]; for(int rt=0;rt<8;rt++) if((target>>(rt*4))&15) sb_f(D,"layout(location=%d) out %s frag_color%d;\n",rt,((si->rt_int_mask>>rt)&1)? (((si->rt_sint_mask>>rt)&1)? "ivec4" : "uvec4") : "vec4",rt);
  }
  if(c->uses_local) sb_put(D,"uint lmem[1024];\n");
  if(c->uses_shared) sb_f(D,"shared uint smem[%d];\n", stage==ST_CS && si->shared_size>0? (si->shared_size+3)/4 : 16384);
  if(stage==ST_CS){ static const char* it[6]={"image1D","imageBuffer","image1DArray","image2D","image2DArray","image3D"};
    for(int k=0;k<si->nimg;k++) sb_f(D,"layout(binding=%d) writeonly uniform %s img%d;\n",k,it[si->img[k].type],k);
    if(c->uses_img_raw){
      /* format class: 0 f32, 1 f16, 2 unorm8, 3 unorm16 */
      sb_put(D,"uniform int swr_ic[8];\nuniform int swr_tc[32];\nuniform int swr_ib[8];\nuniform int swr_tb[32];\n"
        "vec4 swr_r2v(uvec4 r, uint n, uint sz, int cls){\n"
        "  if(sz<4u){ uint v = sz<2u? (r.x&0xFFu) : (r.x&0xFFFFu); if(cls==1) return vec4(unpackHalf2x16(v).x,0.0,0.0,1.0); if(cls==2) return vec4(float(v&0xFFu)/255.0,0.0,0.0,1.0); if(cls==3) return vec4(float(v)/65535.0,0.0,0.0,1.0); return vec4(uintBitsToFloat(v),0.0,0.0,1.0); }\n"
        "  if(cls==1) return n>=2u? vec4(unpackHalf2x16(r.x),unpackHalf2x16(r.y)) : vec4(unpackHalf2x16(r.x),0.0,1.0);\n"
        "  if(cls==2) return unpackUnorm4x8(r.x);\n"
        "  if(cls==3) return n>=2u? vec4(unpackUnorm2x16(r.x),unpackUnorm2x16(r.y)) : vec4(unpackUnorm2x16(r.x),0.0,1.0);\n"
        "  return vec4(uintBitsToFloat(r.x), n>=2u? uintBitsToFloat(r.y) : 0.0, n>=4u? uintBitsToFloat(r.z) : 0.0, n>=4u? uintBitsToFloat(r.w) : 1.0); }\n"
        "uvec4 swr_v2r(vec4 v, uint sz, int cls){\n"
        "  uvec4 o;\n"
        "  if(cls==1) o=uvec4(packHalf2x16(v.xy),packHalf2x16(v.zw),0u,0u); else if(cls==2) o=uvec4(packUnorm4x8(v),0u,0u,0u);\n"
        "  else if(cls==3) o=uvec4(packUnorm2x16(v.xy),packUnorm2x16(v.zw),0u,0u); else o=floatBitsToUint(v);\n"
        "  if(sz<2u) o.x&=0xFFu; else if(sz<4u) o.x&=0xFFFFu; return o; }\n"); } }
  /* FMZ multiply: 0 * anything = 0, inf/NaN included */
  { const char* b=c->out.p? c->out.p : ""; int f1=0, f2=strstr(b,"ffmz(")!=NULL;   /* only when needed */
    for(const char* q=b; (q=strstr(q,"fmz(")); q++) if(q==b || q[-1]!='f'){ f1=1; break; }
    if(f1) sb_put(D,"float fmz(float a, float b){ return (a==0.0 || b==0.0) ? 0.0 : a*b; }\n");
    if(f2) sb_put(D,"float ffmz(float a, float b, float c){ return (a==0.0 || b==0.0) ? c : fma(a,b,c); }\n"); }
  sb_put(D,"void main(){\n");
  /* Position components omitted by a vertex shader must remain deterministic.
     Screen-space passes commonly write only X/Y; undefined Z/W can clip a
     whole quad or one triangle depending on preceding GPU execution. */
  if(c->stage==ST_VS) sb_put(D,"  gl_Position = vec4(0.0, 0.0, 0.0, 1.0);\n");
  if(c->stage==ST_VS && !g_glsl_es && !c->si->writes_point_size) sb_put(D,"  gl_PointSize = 1.0;\n");
  /* VS regs are precise. depth prepass and color pass use different shaders with the same position math;
     if the driver fuses an fma in only one of them, depths differ by an ulp and the EQUAL test fails */
  { static int nop=-1; if(nop<0) nop=getenv("SWR_NO_PRECISE_VS")!=NULL;
    const char* rq= (c->stage==ST_VS && !g_glsl_es && !nop)? "precise uint" : "uint";
    for(int i=0;i<255;i++) if(c->used_reg[i]) sb_f(D,"  %s r%d = 0u;\n",rq,i); }
  for(int i=0;i<7;i++) if(c->used_pred[i]) sb_f(D,"  bool p%d = false;\n",i);
  if(c->uses_cc) sb_put(D,"  bool cc_z=false, cc_s=false, cc_c=false;\n");
  if(c->uses_stk[0]) sb_put(D,"  uint ssy[16]; int ssy_sp=0;\n");
  if(c->uses_stk[1]) sb_put(D,"  uint pbk[16]; int pbk_sp=0;\n");
  if(c->uses_stk[2]) sb_put(D,"  uint pcnt[16]; int pcnt_sp=0;\n");
  if(stage==ST_FS){ u32 target=si->sph[18]; for(int rt=0;rt<8;rt++) if((target>>(rt*4))&15) sb_f(D,"  frag_color%d = %s(0);\n",rt,((si->rt_int_mask>>rt)&1)? (((si->rt_sint_mask>>rt)&1)? "ivec4" : "uvec4") : "vec4"); }
  sb_put(D,c->out.p? c->out.p : ""); sb_put(D,"}\n");
  char* r=D->p; free(c->out.p); free(c->pop_tgt); free(c);
  return r;
}
