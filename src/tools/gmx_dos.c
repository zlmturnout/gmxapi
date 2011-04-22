/* -*- mode: c; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4; c-file-style: "stroustrup"; -*- 
 * 
 *                This source code is part of
 * 
 *                 G   R   O   M   A   C   S
 * 
 *          GROningen MAchine for Chemical Simulations
 * 
 *                        VERSION 3.2.0
 * Written by David van der Spoel, Erik Lindahl, Berk Hess, and others.
 * Copyright (c) 1991-2000, University of Groningen, The Netherlands.
 * Copyright (c) 2001-2004, The GROMACS development team,
 * check out http://www.gromacs.org for more information.

 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 * 
 * If you want to redistribute modifications, please consider that
 * scientific software is very special. Version control is crucial -
 * bugs must be traceable. We will be happy to consider code for
 * inclusion in the official distribution, but derived work must not
 * be called official GROMACS. Details are found in the README & COPYING
 * files - if they are missing, get the official version at www.gromacs.org.
 * 
 * To help us fund GROMACS development, we humbly ask that you cite
 * the papers on the package - you can find them in the top README file.
 * 
 * For more info, check our website at http://www.gromacs.org
 * 
 * And Hey:
 * Green Red Orange Magenta Azure Cyan Skyblue
 */
#ifdef HAVE_CONFIG_H
#include <config.h>
#endif
#include <stdio.h>
#include <math.h>

#include "confio.h"
#include "copyrite.h"
#include "gmx_fatal.h"
#include "futil.h"
#include "gstat.h"
#include "macros.h"
#include "maths.h"
#include "physics.h"
#include "index.h"
#include "smalloc.h"
#include "statutil.h"
#include "string.h"
#include "sysstuff.h"
#include "txtdump.h"
#include "typedefs.h"
#include "vec.h"
#include "strdb.h"
#include "xvgr.h"
#include "correl.h"
#include "gmx_ana.h"
#include "gmx_fft.h"

static real FD(real Delta,real f)
{
    return (2*pow(Delta,-4.5)*pow(f,7.5) - 
            6*pow(Delta,-3)*pow(f,5) -
            pow(Delta,-1.5)*pow(f,3.5) +
            6*pow(Delta,-1.5)*pow(f,2.5) +
            2*f - 2);
}

static real calc_fluidicity(real Delta,real tol)
{
    real fd0,fd,fd1,f,f0=0,f1=1;
    real tolmin = 1e-6;
    
    /* Since the fluidity is monotonous according to Fig. 2 in Lin2003a, 
       J. Chem. Phys. 112 (2003) p. 11792 we can use a bisection method
       to get it. */
    if (tol < tolmin) 
    {
        fprintf(stderr,"Unrealistic tolerance %g for calc_fluidity. Setting it to %g\n",tol,tolmin);
        tol=1e-6;
    }
    
    do {
        fd0 = FD(Delta,f0);
        fd1 = FD(Delta,f1);
        f = (f0+f1)*0.5;
        fd = FD(Delta,f);
        if (fd < 0)
            f0 = f;
        else if (fd > 0)
            f1 = f;
        else
            return f;
    } while ((f1-f0) > tol);
    
    return f;
}

static real wCsolid(real nu,real beta)
{
    real bhn = beta*PLANCK*nu;
    real ebn;
    
    if (bhn == 0)
        return 1;
    else 
    {
        ebn = exp(-beta*PLANCK*nu);
        
        return ebn/sqr(1-ebn);
    }
}

static real wSsolid(real nu,real beta)
{
    real bhn = beta*PLANCK*nu;
    real ebn;
    
    if (bhn == 0) 
    {
        return 1;
    }
    else 
    {
        ebn = exp(-bhn);
        
        return bhn/(exp(bhn)-1) - log(1-exp(-bhn));
    }
}

int gmx_dos(int argc,char *argv[])
{
    const char *desc[] = {
        "[TT]g_dos[tt] computes the Density of States from a simulations.",
        "In order for this to be meaningful the velocities must be saved",
        "in the trajecotry with sufficiently high frequency such as to cover",
        "all vibrations. For flexible systems that would be around a few fs",
        "between saving. Properties based on the DoS are printed on the",
        "standard output."
    };
    const char *bugs[] = {
        "This program needs a lot of memory: total usage equals the number of atoms times 3 times number of frames times 4 (or 8 when run in double precision)."
    };
    FILE       *fp;
    t_topology top;
    int        ePBC=-1;
    t_trxframe fr;
    matrix     box;
    int        gnx;
    char       title[256];
    real       t0,t1,m;
    t_trxstatus *status;
    int        nV,teller,n_alloc,i,j,k,l,fftcode,Nmol,Natom;
    double     dt,V2sum,Vsum,V,tmass;
    real       **c1,**dos,mi,beta,*nu,stddev;
    output_env_t oenv;
    gmx_fft_t  fft;
    double     cP,S,Delta,f,DoS0;
    double     wCdiff,wSdiff;
    
    static     gmx_bool bVerbose=TRUE;
    static     real Temp=298.15,toler=1e-3;
    t_pargs pa[] = {
        { "-v", FALSE, etBOOL, {&bVerbose},
          "Be loud and noisy" },
        { "-T", FALSE, etREAL, {&Temp},
          "Temperature in the simulation" },
        { "-toler", FALSE, etREAL, {&toler},
          "[HIDDEN]Tolerance when computing the fluidicity using bisection algorithm" }
    };

    t_filenm  fnm[] = {
        { efTRN, "-f",    NULL,   ffREAD  },
        { efTPX, "-s",    NULL,   ffREAD }, 
        { efNDX, NULL,    NULL,   ffOPTRD },
        { efXVG, "-acf",  "acf",  ffWRITE },
        { efXVG, "-dos",  "dos",  ffWRITE }
    };
#define NFILE asize(fnm)
    int     npargs;
    t_pargs *ppa;
    const char *DoSlegend[] = {
        "DoS(v)", "DoS(v)[Solid]", "DoS(v)[Diff]" 
    };
    
    CopyRight(stderr,argv[0]);
    npargs = asize(pa);
    ppa    = add_acf_pargs(&npargs,pa);
    parse_common_args(&argc,argv,PCA_CAN_VIEW | PCA_CAN_TIME | PCA_BE_NICE,
                      NFILE,fnm,npargs,ppa,asize(desc),desc,
                      asize(bugs),bugs,&oenv);
                      
    please_cite(stdout,"Pascal2011a");
    please_cite(stdout,"Caleman2011b");
    
    read_tps_conf(ftp2fn(efTPX,NFILE,fnm),title,&top,&ePBC,NULL,NULL,box,
                  TRUE);
    V = det(box);
    tmass = 0;
    for(i=0; (i<top.atoms.nr); i++)
        tmass += top.atoms.atom[i].m;

    Natom = top.atoms.nr;
    Nmol = top.mols.nr;
    gnx = Natom*DIM;
    
    /* Correlation stuff */
    snew(c1,gnx);
    for(i=0; (i<gnx); i++)
        c1[i]=NULL;
  
    read_first_frame(oenv,&status,ftp2fn(efTRN,NFILE,fnm),&fr,TRX_NEED_V);
    t0=fr.time;
      
    n_alloc=0;
    teller=0;
    Vsum = V2sum = 0;
    nV = 0;
    do {
        if (fr.bBox) 
        {
            V = det(fr.box);
            V2sum += V*V;
            Vsum += V;
            nV++;
        }
        if (teller >= n_alloc) 
        {
            n_alloc+=100;
            for(i=0; i<gnx; i++)
                srenew(c1[i],n_alloc);
        }
        for(i=0; i<gnx; i+=DIM) 
        {
            c1[i+XX][teller] = fr.v[i/DIM][XX];
            c1[i+YY][teller] = fr.v[i/DIM][YY];
            c1[i+ZZ][teller] = fr.v[i/DIM][ZZ];
        }

        t1=fr.time;

        teller ++;
    } while (read_next_frame(oenv,status,&fr));
  
    close_trj(status);

    dt = (t1-t0)/(teller-1);
    if (nV > 0)
    {
        V = Vsum/nV;
    }
    if (bVerbose)
        printf("Going to do %d fourier transforms of length %d. Hang on.\n",
               gnx,teller);
    low_do_autocorr(NULL,oenv,NULL,teller,gnx,teller,c1,dt,eacNormal,0,FALSE,
                    FALSE,FALSE,-1,-1,0,0);
    snew(dos,6);
    for(j=0; (j<6); j++)
        snew(dos[j],teller);

    if (bVerbose)
        printf("Going to merge the ACFs into the mass-weighted total ACF\n");
    for(i=0; (i<gnx); i+=DIM) 
    {
        mi = top.atoms.atom[i/DIM].m;
        for(j=0; (j<teller/2); j++) 
            dos[0][j] += mi * (c1[i+XX][j] + c1[i+YY][j] + c1[i+ZZ][j]);
    }
    fp = xvgropen(opt2fn("-acf",NFILE,fnm),"Mass-weighted velocity ACF",
                  "Time (ps)","C(t)",oenv);
    for(j=0; (j<teller/2); j++) 
        fprintf(fp,"%10g  %10g\n",j*dt,dos[0][j]);
    fclose(fp);
    
    if ((fftcode = gmx_fft_init_1d_real(&fft,teller/2,GMX_FFT_FLAG_NONE)) != 0) 
    {
        gmx_fatal(FARGS,"gmx_fft_init_1d_real returned %d",fftcode);
    }
    if ((fftcode = gmx_fft_1d_real(fft,GMX_FFT_REAL_TO_COMPLEX,
                                   (void *)dos[0],(void *)dos[1])) != 0)
    {
        gmx_fatal(FARGS,"gmx_fft_1d_real returned %d",fftcode);
    }

    /* First compute the DoS */
    beta = 1/(Temp*BOLTZ);
    for(j=0; (j<teller/2); j++) 
    {
        dos[1][j] = (beta/2)*dos[1][2*j];
    }
    /* Now analyze it */
    DoS0 = dos[1][0];
    Delta = ((2*DoS0/(9*Natom))*pow(M_PI*BOLTZ*Temp/tmass,1.0/3.0)*
             pow((Natom/V),1.0/3.0)*pow(6/M_PI,2.0/3.0));
    f = calc_fluidicity(Delta,toler);
    printf("Nmol = %d, Natom = %d, dt = %g ps\n"
           "V = %g nm^3, Delta = %g, f = %g\n",
           Nmol,Natom,dt,V,Delta,f);
           
    snew(nu,teller/2);
    /* Now compute solid (2) and diffusive (3) components */
    fp = xvgropen(opt2fn("-dos",NFILE,fnm),"Density of states",
                  "\\f{12}n\\f{4} (1/ps)","\\f{4}S(\\f{12}n\\f{4})",oenv);
    xvgr_legend(fp,asize(DoSlegend),DoSlegend,oenv);
    for(j=0; (j<teller/2); j++) 
    {
        nu[j] = j/dt;
        dos[3][j] = DoS0/(1+sqr(DoS0*M_PI*nu[j]/(6*f*Natom)));
        dos[2][j] = dos[1][j]-dos[3][j];
        fprintf(fp,"%10g  %10g  %10g  %10g\n",nu[j],dos[1][j],
                dos[2][j],dos[3][j]);
    }
    fclose(fp);

    /* Finally analyze the results! */    
    wCdiff = 0.5;
    wSdiff = DoS0/(3*BOLTZ); /* Is this correct? */
    for(j=0; (j<teller/2); j++) 
    {
        dos[4][j] = dos[3][j]*wCdiff + dos[4][j]*wCsolid(nu[j],beta);
        dos[5][j] = dos[3][j]*wSdiff + dos[4][j]*wSsolid(nu[j],beta);
    }  
    cP = BOLTZ * evaluate_integral(teller/2,nu,dos[4],NULL,
                                   teller/4,&stddev);
    printf("Heat capacity %g kJ/mol K\n",cP);
    
    S  = BOLTZ * evaluate_integral(teller/2,nu,dos[5],NULL,
                                   teller/4,&stddev);
    printf("Entropy %g kJ/mol K\n",S);
    
    do_view(oenv,ftp2fn(efXVG,NFILE,fnm),"-nxy");
  
    thanx(stderr);
  
    return 0;
}
