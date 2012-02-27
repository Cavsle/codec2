/*---------------------------------------------------------------------------*\

  FILE........: c2sim.c
  AUTHOR......: David Rowe
  DATE CREATED: 20/8/2010

  Codec2 simulation.  Combines encoder and decoder and allows
  switching in and out various algorithms and quantisation
  steps. Used for algorithm development.

\*---------------------------------------------------------------------------*/

/*
  Copyright (C) 2009 David Rowe

  All rights reserved.

  This program is free software; you can redistribute it and/or modify
  it under the terms of the GNU Lesser General Public License version 2.1, as
  published by the Free Software Foundation.  This program is
  distributed in the hope that it will be useful, but WITHOUT ANY
  WARRANTY; without even the implied warranty of MERCHANTABILITY or
  FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public
  License for more details.

  You should have received a copy of the GNU Lesser General Public License
  along with this program; if not, see <http://www.gnu.org/licenses/>.
*/

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <math.h>
#include <unistd.h>
#include <getopt.h>

#include "defines.h"
#include "sine.h"
#include "nlp.h"
#include "dump.h"
#include "lpc.h"
#include "lsp.h"
#include "quantise.h"
#include "phase.h"
#include "postfilter.h"
#include "interp.h"

void synth_one_frame(short buf[], MODEL *model, float Sn_[], float Pn[]);
void print_help(const struct option *long_options, int num_opts, char* argv[]);

/*---------------------------------------------------------------------------*\
                                                                          
				MAIN                                        
                                                                         
\*---------------------------------------------------------------------------*/

int main(int argc, char *argv[])
{
    FILE *fout = NULL;	/* output speech file                    */
    FILE *fin;		/* input speech file                     */
    short buf[N];		/* input/output buffer                   */
    float Sn[M];	        /* float input speech samples            */
    COMP  Sw[FFT_ENC];	/* DFT of Sn[]                           */
    float w[M];	        /* time domain hamming window            */
    COMP  W[FFT_ENC];	/* DFT of w[]                            */
    MODEL model;
    float Pn[2*N];	/* trapezoidal synthesis window          */
    float Sn_[2*N];	/* synthesised speech */
    int   i;		/* loop variable                         */
    int   frames;
    float prev_Wo, prev__Wo, uq_Wo, prev_uq_Wo;
    float pitch;
    int   voiced1 = 0;
    char  out_file[MAX_STR];
    float snr;
    float sum_snr;

    int lpc_model = 0, order = LPC_ORD;
    int lsp = 0, lspd = 0, lspvq = 0;
    int lspres = 0;
    int lspdt = 0, lspdt_mode = LSPDT_ALL;
    int dt = 0, lspjvm = 0, lspjnd = 0;
    float ak[LPC_MAX];
    COMP  Sw_[FFT_ENC];
    COMP  Ew[FFT_ENC]; 
 
    int dump = 0;
  
    int phase0 = 0;
    float ex_phase[MAX_AMP+1];

    int   postfilt;
    float bg_est;

    int   hand_voicing = 0, phasetest = 0;
    FILE *fvoicing = 0;

    MODEL prev_model, interp_model;
    int decimate = 0;
    float lsps[LPC_ORD];
    float prev_lsps[LPC_ORD], prev_lsps_[LPC_ORD];
    float lsps__prev[LPC_ORD];
    float lsps__prev2[LPC_ORD];
    float e, prev_e;
    float ak_interp[LPC_MAX];
    int   lsp_indexes[LPC_MAX];
    float lsps_[LPC_ORD];

    void *nlp_states;
    float hpf_states[2];

    char* opt_string = "ho:";
    struct option long_options[] = {
        { "lpc", required_argument, &lpc_model, 1 },
        { "lspjnd", no_argument, &lspjnd, 1 },
        { "lsp", no_argument, &lsp, 1 },
        { "lspd", no_argument, &lspd, 1 },
        { "lspvq", no_argument, &lspvq, 1 },
        { "lspres", no_argument, &lspres, 1 },
        { "lspdt", no_argument, &lspdt, 1 },
        { "lspdt_mode", required_argument, NULL, 0 },
        { "lspjvm", no_argument, &lspjvm, 1 },
        { "phase0", no_argument, &phase0, 1 },
        { "phasetest", no_argument, &phasetest, 1 },
        // { "postfilter", no_argument, &postfilter, 1 },
        { "hand_voicing", required_argument, &hand_voicing, 1 },
        { "dec", no_argument, &decimate, 1 },
        { "dt", no_argument, &dt, 1 },
        { "rate", required_argument, NULL, 0 },
#ifdef DUMP
        { "dump", required_argument, &dump, 1 },
#endif
        { "help", no_argument, NULL, 'h' },
        { NULL, no_argument, NULL, 0 }
    };
    int num_opts=sizeof(long_options)/sizeof(struct option);
    
    for(i=0; i<M; i++)
	Sn[i] = 1.0;
    for(i=0; i<2*N; i++)
	Sn_[i] = 0;

    prev_uq_Wo = prev_Wo = prev__Wo = TWO_PI/P_MAX;

    prev_model.Wo = TWO_PI/P_MIN;
    prev_model.L = floor(PI/prev_model.Wo);
    for(i=1; i<=prev_model.L; i++) {
	prev_model.A[i] = 0.0;
	prev_model.phi[i] = 0.0;
    }
    for(i=1; i<=MAX_AMP; i++) {
	//ex_phase[i] = (PI/3)*(float)rand()/RAND_MAX;
	ex_phase[i] = 0.0;
    }
    for(i=0; i<LPC_ORD; i++) {
	lsps_[i] = prev_lsps[i] = prev_lsps_[i] = i*PI/(LPC_ORD+1);
	lsps__prev[i] = lsps__prev2[i] = i*PI/(LPC_ORD+1);
    }
    e = prev_e = 1;
    hpf_states[0] = hpf_states[1] = 0.0;

    nlp_states = nlp_create();

    if (argc < 2) {
        print_help(long_options, num_opts, argv);
    }

    /* Interpret command line arguments -------------------------------------*/

    /* Arguments */
    while(1) {
        int option_index = 0;
        int opt = getopt_long(argc, argv, opt_string, 
                    long_options, &option_index);
        if (opt == -1)
            break;
        switch (opt) {
         case 0:
            if(strcmp(long_options[option_index].name, "lpc") == 0) {
                order = atoi(optarg);
                if((order < 4) || (order > 20)) {
                    fprintf(stderr, "Error in LPC order: %s\n", optarg);
                    exit(1);
                }
#ifdef DUMP
            } else if(strcmp(long_options[option_index].name, "dump") == 0) {
                if (dump) 
	            dump_on(optarg);
#endif
            } else if(strcmp(long_options[option_index].name, "lsp") == 0
                  || strcmp(long_options[option_index].name, "lspd") == 0
                  || strcmp(long_options[option_index].name, "lspvq") == 0) {
	        assert(order == LPC_ORD);
            } else if(strcmp(long_options[option_index].name, "lspdt_mode") == 0) {
	        if (strcmp(optarg,"all") == 0)
	            lspdt_mode = LSPDT_ALL;
	        else if (strcmp(optarg,"low") == 0)
	            lspdt_mode = LSPDT_LOW;
	        else if (strcmp(optarg,"high") == 0)
	            lspdt_mode = LSPDT_HIGH;
	        else {
	            fprintf(stderr, "Error in lspdt_mode: %s\n", optarg);
	            exit(1);
	        }
            } else if(strcmp(long_options[option_index].name, "hand_voicing") == 0) {
	        if ((fvoicing = fopen(optarg,"rt")) == NULL) {
	            fprintf(stderr, "Error opening voicing file: %s: %s.\n",
		        optarg, strerror(errno));
                    exit(1);
                }
            } else if(strcmp(long_options[option_index].name, "rate") == 0) {
                if(strcmp(optarg,"2500") == 0) {
	            lpc_model = 1; order = 10;
	            lsp = 1;
	            phase0 = 1;
	            postfilt = 1;
	            decimate = 1;
                } else if(strcmp(optarg,"1500") == 0) {
	            lpc_model = 1; order = 10;
	            lsp = 1; lspdt = 1;
	            phase0 = 1;
	            postfilt = 1;
	            decimate = 1;
	            dt = 1;
                } else if(strcmp(optarg,"1200") == 0) {
	            lpc_model = 1; order = 10;
	            lspjvm = 1; lspdt = 1;
	            phase0 = 1;
	            postfilt = 1;
	            decimate = 1;
	            dt = 1;
                } else {
                    fprintf(stderr, "Error: invalid output rate %s\n", optarg);
                    exit(1);
                }
            }
            break;

         case 'h':
            print_help(long_options, num_opts, argv);
            break;

         case 'o':
	    if ((fout = fopen(optarg,"wb")) == NULL) {
	        fprintf(stderr, "Error opening output speech file: %s: %s.\n",
		    optarg, strerror(errno));
	        exit(1);
	    }
	    strcpy(out_file,optarg);
            break;

         default:
            /* This will never be reached */
            break;
        }
    }

    /* Input file */

    if ((fin = fopen(argv[optind],"rb")) == NULL) {
	fprintf(stderr, "Error opening input speech file: %s: %s.\n",
		argv[optind], strerror(errno));
	exit(1);
    }

    ex_phase[0] = 0;
    bg_est = 0.0;

    /*
      printf("lspd: %d lspdt: %d lspdt_mode: %d  phase0: %d postfilt: %d "
	   "decimate: %d dt: %d\n",lspd,lspdt,lspdt_mode,phase0,postfilt,
	   decimate,dt);
    */
    /* Initialise ------------------------------------------------------------*/

    make_analysis_window(w,W);
    make_synthesis_window(Pn);
    quantise_init();

    /* Main Loop ------------------------------------------------------------*/

    frames = 0;
    sum_snr = 0;
    while(fread(buf,sizeof(short),N,fin)) {
	frames++;
	//printf("frame: %d\n", frames);

	/* Read input speech */

	for(i=0; i<M-N; i++)
	    Sn[i] = Sn[i+N];
	for(i=0; i<N; i++)
	    Sn[i+M-N] = buf[i];
 
	/* Estimate pitch */

	nlp(nlp_states,Sn,N,M,P_MIN,P_MAX,&pitch,Sw,&prev_uq_Wo);
	model.Wo = TWO_PI/pitch;
	
	/* estimate model parameters --------------------------------------*/

	dft_speech(Sw, Sn, w); 
	two_stage_pitch_refinement(&model, Sw);
	estimate_amplitudes(&model, Sw, W);
	uq_Wo = model.Wo;
#ifdef DUMP 
	dump_Sn(Sn); dump_Sw(Sw); dump_model(&model);
#endif

	/* optional zero-phase modelling ----------------------------------*/

	if (phase0) {
	    float Wn[M];		        /* windowed speech samples */
	    float Rk[LPC_MAX+1];	        /* autocorrelation coeffs  */
	    int ret;

#ifdef DUMP
	    dump_phase(&model.phi[0], model.L);
#endif

	    /* find aks here, these are overwritten if LPC modelling is enabled */

	    for(i=0; i<M; i++)
		Wn[i] = Sn[i]*w[i];
	    autocorrelate(Wn,Rk,M,order);
	    levinson_durbin(Rk,ak,order);

#ifdef DUMP
	    dump_ak(ak, LPC_ORD);
#endif
	
	    /* determine voicing */

	    snr = est_voicing_mbe(&model, Sw, W, Sw_, Ew, prev_uq_Wo);

	    //printf("snr %3.2f v: %d Wo: %f prev_Wo: %f\n", snr, model.voiced,
	    //	   model.Wo, prev_uq_Wo);
#ifdef DUMP
	    dump_Sw_(Sw_);
	    dump_Ew(Ew);
	    dump_snr(snr);
#endif

	    /* just to make sure we are not cheating - kill all phases */

	    for(i=0; i<MAX_AMP; i++)
	    	model.phi[i] = 0;
	
	    if (hand_voicing) {
		ret = fscanf(fvoicing,"%d\n",&model.voiced);
	    }
	}

	/* optional LPC model amplitudes and LSP quantisation -----------------*/

	if (lpc_model) {

	    e = speech_to_uq_lsps(lsps, ak, Sn, w, order);

#ifdef DUMP
	    /* dump order is different if we are decimating */
	    if (!decimate)
		dump_lsp(lsps);
	    for(i=0; i<LPC_ORD; i++)
		prev_lsps[i] = lsps[i];
#endif

	    /* various LSP quantisation schemes */

	    if (lsp) {
		encode_lsps_scalar(lsp_indexes, lsps, LPC_ORD);
		decode_lsps_scalar(lsps_, lsp_indexes, LPC_ORD);
		bw_expand_lsps(lsps_, LPC_ORD);
		lsp_to_lpc(lsps_, ak, LPC_ORD);
	    }

	    if (lspd) {
		lspd_quantise(lsps, lsps_, LPC_ORD);
		bw_expand_lsps(lsps_, LPC_ORD);
		lsp_to_lpc(lsps_, ak, LPC_ORD);
	    }

	    if (lspvq) {
		lspvq_quantise(lsps, lsps_, LPC_ORD);
		bw_expand_lsps(lsps_, LPC_ORD);
		lsp_to_lpc(lsps_, ak, LPC_ORD);
	    }

	    if (lspjvm) {
		/* Jean-marcs multi-stage VQ */
		lspjvm_quantise(lsps, lsps_, LPC_ORD);
		bw_expand_lsps(lsps_, LPC_ORD);			    
		lsp_to_lpc(lsps_, ak, LPC_ORD);
	    }

	    /* we need lsp__prev[] for lspdt and decimate.  If no
	       other LSP quantisation is used we use original LSPs as
	       there is no quantised version available. */

	    if (!lsp && !lspd && !lspvq && !lspres && !lspjvm)
		for(i=0; i<LPC_ORD; i++)
		    lsps_[i] = lsps[i];

	    if (lspjnd) {
		//locate_lsps_jnd_steps(lsps, LPC_ORD);
		lspjnd_quantise(lsps, lsps_, LPC_ORD);
		bw_expand_lsps(lsps_, LPC_ORD);
		lsp_to_lpc(lsps_, ak, LPC_ORD);
	    }

	    /* Odd frames are generated by quantising the difference
	       between the previous frames LSPs and this frames */
	
	    if (lspdt && !decimate) {
		if (frames%2) {
		    lspdt_quantise(lsps, lsps_, lsps__prev, lspdt_mode);
		    bw_expand_lsps(lsps_, LPC_ORD);
		    lsp_to_lpc(lsps_, ak, LPC_ORD);
		}
		for(i=0; i<LPC_ORD; i++)
		    lsps__prev[i] = lsps_[i];		
	    }

	    /* 
	       When decimation is enabled we only send LSPs to the
	       decoder on odd frames.  In the Delta-time LSPs case we
	       encode every second odd frame (i.e. every 3rd frame out
	       of 4) by quantising the difference between the 1st
	       frames LSPs and the 3rd frames:

	       10ms, frame 1: discard (interpolate at decoder)
	       20ms, frame 2: send "full" LSP frame
	       30ms, frame 3: discard (interpolate at decoder)
	       40ms, frame 4: send LSPs differences between frame 4 and frame 2
	    */
   
	    if (lspdt && decimate) {
		/* print previous LSPs to make sure we are using the right set */
		if ((frames%4) == 0) {
		    //printf("  lspdt ");  
		    //lspdt_quantise(lsps, lsps_, lsps__prev2, lspdt_mode);
		    for(i=0; i<LPC_ORD; i++)
			lsps_[i] = lsps__prev2[i];		  
		    bw_expand_lsps(lsps_, LPC_ORD);
		    lsp_to_lpc(lsps_, ak, LPC_ORD);
		}
		
		for(i=0; i<LPC_ORD; i++) {
		    lsps__prev2[i] = lsps__prev[i];
		    lsps__prev[i] = lsps_[i];
		}
	    }
#ifdef DUMP
	    /* if using decimated (20ms) frames we dump interp
	       LSPs below */
	    if (!decimate)
		dump_lsp(lsps_);
#endif
	
	    /* We quantise energy and Wo when doing any form of LPC
	       modelling.  This sounds transparent and saves an extra 
	       command line switch to enable/disable. */

	    e = decode_energy(encode_energy(e));

	    if (!decimate) {
		/* we send params every 10ms, delta-time every 20ms */
		if (dt && (frames % 2)) 
		    model.Wo = decode_Wo_dt(encode_Wo_dt(model.Wo, prev_Wo),prev_Wo);
		else
		    model.Wo = decode_Wo(encode_Wo(model.Wo));
	    }

	    if (decimate) {
		/* we send params every 20ms */
		if (dt && ((frames % 4) == 0)) {
		    /* delta-time every 40ms */
		    model.Wo = decode_Wo_dt(encode_Wo_dt(model.Wo, prev__Wo),prev__Wo);
		}
		else
		    model.Wo = decode_Wo(encode_Wo(model.Wo));		    
	    }

	    model.L  = PI/model.Wo; /* if we quantise Wo re-compute L */
	   
	    aks_to_M2(ak, order, &model, e, &snr, 1); 
	    apply_lpc_correction(&model);

	    /* note SNR on interpolated frames can't be measured properly
	       by comparing Am as L has changed.  We can dump interp lsps
	       and compare them,
	    */
#ifdef DUMP
	    dump_lpc_snr(snr);
#endif
	    sum_snr += snr;
#ifdef DUMP
	    dump_quantised_model(&model);
#endif
	}

#ifdef RESAMPLE
	/* optional resampling of model amplitudes */

	//printf("frames=%d\n", frames);
	if (resample) {
	    snr = resample_amp_nl(&model, resample, AresdB_prev);
	    sum_snr += snr;
#ifdef DUMP
	    dump_quantised_model(&model);
#endif
	}
#endif

	/* option decimation to 20ms rate, which enables interpolation
	   routine to synthesise in between frame ---------------------------*/
  
	if (decimate) {
	    float lsps_interp[LPC_ORD];

	    if (!phase0) {
		printf("needs --phase0 to resample phase for interpolated Wo\n");
		exit(0);
	    }
	    if (!lpc_model) {
		printf("needs --lpc 10 to resample amplitudes\n");
		exit(0);
	    }

	    /* 
	       Each 20ms we synthesise two 10ms frames:

	       frame 1: discard except for voicing bit
	       frame 2: interpolate frame 1 LSPs from frame 2 and frame 0
	                synthesise frame 1 and frame 2 speech
	       frame 3: discard except for voicing bit
	       frame 4: interpolate frame 3 LSPs from frame 4 and frame 2
	                synthesise frame 3 and frame 4 speech
	    */

	    if ((frames%2) == 0) {
		//printf("frame: %d\n", frames);

		/* decode interpolated frame */

		interp_model.voiced = voiced1;
		
#ifdef LOG_LIN_INTERP
		interpolate(&interp_model, &prev_model, &model);
#else
		interpolate_lsp(&interp_model, &prev_model, &model,
				prev_lsps_, prev_e, lsps_, e, ak_interp, lsps_interp);		
		apply_lpc_correction(&interp_model);

		/* used to compare with c2enc/c2dec version 

		printf("  Wo: %1.5f  L: %d v1: %d prev_e: %f\n", 
		       interp_model.Wo, interp_model.L, interp_model.voiced, prev_e);
		printf("  lsps_interp: ");
		for(i=0; i<LPC_ORD; i++)
		    printf("%5.3f  ", lsps_interp[i]);
		printf("\n  A..........: ");
		for(i=0; i<10; i++)
		    printf("%5.3f  ",interp_model.A[i]);

		printf("\n  Wo: %1.5f  L: %d e: %3.2f v2: %d\n", 
		       model.Wo, model.L, e, model.voiced);
		printf("  lsps_......: ");
		for(i=0; i<LPC_ORD; i++)
		    printf("%5.3f  ", lsps_[i]);
		printf("\n  A..........: ");
		for(i=0; i<10; i++)
		    printf("%5.3f  ",model.A[i]);
		printf("\n");
		*/
			
#ifdef DUMP
		/* do dumping here so we get lsp dump file in correct order */
		dump_lsp(prev_lsps);
		dump_lsp(lsps_interp);
		dump_lsp(lsps);
		dump_lsp(lsps_);
#endif
#endif
		if (phase0)
		    phase_synth_zero_order(&interp_model, ak_interp, ex_phase,
					   order);	
		if (postfilt)
		    postfilter(&interp_model, &bg_est);
		synth_one_frame(buf, &interp_model, Sn_, Pn);
		//printf("  buf[0] %d\n", buf[0]);
		if (fout != NULL) 
		    fwrite(buf,sizeof(short),N,fout);

		/* decode this frame */

		if (phase0)
		    phase_synth_zero_order(&model, ak, ex_phase, order);	
		if (postfilt)
		    postfilter(&model, &bg_est);
		synth_one_frame(buf, &model, Sn_, Pn);
		//printf("  buf[0] %d\n", buf[0]);
		if (fout != NULL) 
		    fwrite(buf,sizeof(short),N,fout);

		/* update states for next time */

		prev_model = model;
		for(i=0; i<LPC_ORD; i++)
		    prev_lsps_[i] = lsps_[i];
		prev_e = e;
	    }
	    else {
		voiced1 = model.voiced;
	    }
	}
	else {
	    if (phase0)
	    	phase_synth_zero_order(&model, ak, ex_phase, order);	
	    if (postfilt)
		postfilter(&model, &bg_est);
	    synth_one_frame(buf, &model, Sn_, Pn);
	    if (fout != NULL) fwrite(buf,sizeof(short),N,fout);
	}

	prev__Wo = prev_Wo;
	prev_Wo = model.Wo;
	prev_uq_Wo = uq_Wo;
	//if (frames == 8) {
	//    exit(0);
	//}
    }

    /* End Main Loop -----------------------------------------------------*/

    fclose(fin);

    if (fout != NULL)
	fclose(fout);

    if (lpc_model)
	printf("SNR av = %5.2f dB\n", sum_snr/frames);

#ifdef DUMP
    if (dump)
	dump_off();
#endif

    if (hand_voicing)
	fclose(fvoicing);

    nlp_destroy(nlp_states);

    return 0;
}

void synth_one_frame(short buf[], MODEL *model, float Sn_[], float Pn[])
{
    int     i;

    synthesise(Sn_, model, Pn, 1);

    for(i=0; i<N; i++) {
	if (Sn_[i] > 32767.0)
	    buf[i] = 32767;
	else if (Sn_[i] < -32767.0)
	    buf[i] = -32767;
	else
	    buf[i] = Sn_[i];
    }

}

void print_help(const struct option* long_options, int num_opts, char* argv[])
{
	int i;
	char *option_parameters;

	fprintf(stderr, "\nCodec2 - low bit rate speech codec - Simulation Program\n"
		"\thttp://rowetel.com/codec2.html\n\n"
		"usage: %s [OPTIONS] <InputFile>\n\n"
                "Options:\n"
                "\t-o <OutputFile>\n", argv[0]);
        for(i=0; i<num_opts-1; i++) {
		if(long_options[i].has_arg == no_argument) {
			option_parameters="";
		} else if (strcmp("lpc", long_options[i].name) == 0) {
			option_parameters = " <Order>";
		} else if (strcmp("lspdt_mode", long_options[i].name) == 0) {
			option_parameters = " <all|high|low>";
		} else if (strcmp("hand_voicing", long_options[i].name) == 0) {
			option_parameters = " <VoicingFile>";
		} else if (strcmp("rate", long_options[i].name) == 0) {
			option_parameters = " <2500|1500|1200>";
		} else if (strcmp("dump", long_options[i].name) == 0) {
			option_parameters = " <DumpFilePrefix>";
		} else {
			option_parameters = " <UNDOCUMENTED parameter>";
		}
		fprintf(stderr, "\t--%s%s\n", long_options[i].name, option_parameters);
	}
	exit(1);
}