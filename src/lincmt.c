#include <stdio.h>
#include <stdarg.h>
#include <R.h>
#include <Rinternals.h>
#include <Rmath.h>
#include <R_ext/Rdynload.h>
#include "../inst/include/RxODE.h"
#define safe_zero(a) ((a) == 0 ? DOUBLE_EPS : (a))
#define _as_zero(a) (fabs(a) < sqrt(DOUBLE_EPS) ? 0.0 : a)
#define _as_dbleps(a) (fabs(a) < sqrt(DOUBLE_EPS) ? ((a) < 0 ? -sqrt(DOUBLE_EPS)  : sqrt(DOUBLE_EPS)) : a)
#define max2( a , b )  ( (a) > (b) ? (a) : (b) )

#ifdef ENABLE_NLS
#include <libintl.h>
#define _(String) dgettext ("RxODE", String)
/* replace pkg as appropriate */
#else
#define _(String) (String)
#endif

#include "lincmtB1.h"
#include "lincmtB2.h"

// From https://cran.r-project.org/web/packages/Rmpfr/vignettes/log1mexp-note.pdf
double log1mex(double a){
  if (a < M_LN2) return log(-expm1(-a));
  return(log1p(-exp(-a)));
}

void getWh(int evid, int *wh, int *cmt, int *wh100, int *whI, int *wh0);

// Linear compartment models/functions
extern double _getDur(int l, rx_solving_options_ind *ind, int backward, unsigned int *p){
  double dose = ind->dose[l];
  if (backward){
    p[0] = l-1;
    while (p[0] > 0 && ind->dose[p[0]] != -dose){
      p[0]--;
    }
    if (ind->dose[p[0]] != -dose){
      error(_("could not find a start to the infusion"));
    }
    return ind->all_times[ind->idose[l]] - ind->all_times[ind->idose[p[0]]];
  } else {
    p[0] = l+1;
    while (p[0] < ind->ndoses && ind->dose[p[0]] != -dose){
      p[0]++;
    }
    if (ind->dose[p[0]] != -dose){
      error(_("could not find an end to the infusion"));
    }
    return ind->all_times[ind->idose[p[0]]] - ind->all_times[ind->idose[l]];
  }
}


extern double getTime(int idx, rx_solving_options_ind *ind);

extern int _locateTimeIndex(double obs_time,  rx_solving_options_ind *ind){
  // Uses bisection for slightly faster lookup of dose index.
  int i, j, ij;
  i = 0;
  j = ind->n_all_times - 1;
  if (obs_time < getTime(ind->ix[i], ind)){
    return i;
  }
  if (obs_time > getTime(ind->ix[j], ind)){
    return j;
  }
  while(i < j - 1) { /* x[i] <= obs_time <= x[j] */
    ij = (i + j)/2; /* i+1 <= ij <= j-1 */
    if(obs_time < getTime(ind->ix[ij], ind))
      j = ij;
    else
      i = ij;
  }
  /* if (i == 0) return 0; */
  while(i != 0 && obs_time == getTime(ind->ix[i], ind)){
    i--;
  }
  if (i == 0){
    while(i < ind->ndoses-2 && fabs(obs_time  - getTime(ind->ix[i+1], ind))<= sqrt(DOUBLE_EPS)){
      i++;
    }
  }
  return i;
}

/* Authors: Robert Gentleman and Ross Ihaka and The R Core Team */
/* Taken directly from https://github.com/wch/r-source/blob/922777f2a0363fd6fe07e926971547dd8315fc24/src/library/stats/src/approx.c*/
/* Changed as follows:
   - Different Name
   - Use RxODE structure
   - Use getTime to allow model-based changes to dose timing
   - Use getValue to ignore NA values for time-varying covariates
*/
static inline double getValue(int idx, double *y, rx_solving_options_ind *ind){
  int i = idx;
  double ret = y[ind->ix[idx]];
  if (ISNA(ret)){
    // Go backward.
    while (ISNA(ret) && i != 0){
      i--; ret = y[ind->ix[i]];
    }
    if (ISNA(ret)){
      // Still not found go forward.
      i = idx;
      while (ISNA(ret) && i != ind->n_all_times){
	i++; ret = y[ind->ix[i]];
      }
      if (ISNA(ret)){
	// All Covariates values for a single individual are NA.
	ind->allCovWarn=1;
      }
    }
  }
  return ret;
}
#define T(i) getTime(id->ix[i], id)
#define V(i) getValue(i, y, id)
double rx_approxP(double v, double *y, int n,
		  rx_solving_options *Meth, rx_solving_options_ind *id){
  /* Approximate  y(v),  given (x,y)[i], i = 0,..,n-1 */
  int i, j, ij;

  if(!n) return R_NaN;

  i = 0;
  j = n - 1;

  /* handle out-of-domain points */
  if(v < T(i)) return id->ylow;
  if(v > T(j)) return id->yhigh;

  /* find the correct interval by bisection */
  while(i < j - 1) { /* T(i) <= v <= T(j) */
    ij = (i + j)/2; /* i+1 <= ij <= j-1 */
    if(v < T(ij)) j = ij; else i = ij;
    /* still i < j */
  }
  /* provably have i == j-1 */

  /* interpolation */

  if(v == T(j)) return V(j);
  if(v == T(i)) return V(i);
  /* impossible: if(T(j) == T(i)) return V(i); */

  if(Meth->kind == 1){ /* linear */
    return V(i) + (V(j) - V(i)) * ((v - T(i))/(T(j) - T(i)));
  } else { /* 2 : constant */
    return (Meth->f1 != 0.0 ? V(i) * Meth->f1 : 0.0)
      + (Meth->f2 != 0.0 ? V(j) * Meth->f2 : 0.0);
  }
}/* approx1() */

#undef T

/* End approx from R */

// getParCov first(parNo, idx=0) last(parNo, idx=ind->n_all_times-1)
double _getParCov(unsigned int id, rx_solve *rx, int parNo, int idx0){
  rx_solving_options_ind *ind;
  ind = &(rx->subjects[id]);
  rx_solving_options *op = rx->op;
  int idx=0;
  if (idx0 == NA_INTEGER){
    idx=0;
    if (ind->evid[ind->ix[idx]] == 9) idx++;
  } else if (idx0 >= ind->n_all_times) {
    return NA_REAL;
  } else {
    idx=idx0;
  }
  if (idx < 0 || idx > ind->n_all_times) return NA_REAL;
  if (op->do_par_cov){
    for (int k = op->ncov; k--;){
      if (op->par_cov[k] == parNo+1){
	double *y = ind->cov_ptr + ind->n_all_times*k;
	return y[ind->ix[idx]];
      }
    }
  }
  return ind->par_ptr[parNo];
}

void _update_par_ptr(double t, unsigned int id, rx_solve *rx, int idx){
  if (rx == NULL) error(_("solve data is not loaded"));
  if (ISNA(t)){
    rx_solving_options_ind *ind;
    ind = &(rx->subjects[id]);
    rx_solving_options *op = rx->op;
    // Update all covariate parameters
    int k;
    int ncov = op->ncov;
    if (op->do_par_cov){
      for (k = ncov; k--;){
	if (op->par_cov[k]){
	  double *y = ind->cov_ptr + ind->n_all_times*k;
	  ind->par_ptr[op->par_cov[k]-1] = getValue(idx, y, ind);
	  if (idx == 0){
	    ind->cacheME=0;
	  } else if (getValue(idx, y, ind) != getValue(idx-1, y, ind)) {
	    ind->cacheME=0;
	  }
	}
      }
    }
  } else {
    rx_solving_options_ind *ind;
    ind = &(rx->subjects[id]);
    rx_solving_options *op = rx->op;
    // Update all covariate parameters
    int k;
    int ncov = op->ncov;
    if (op->do_par_cov){
      for (k = ncov; k--;){
	if (op->par_cov[k]){
	  double *par_ptr = ind->par_ptr;
	  double *all_times = ind->all_times;
	  double *y = ind->cov_ptr + ind->n_all_times*k;
	  if (idx > 0 && idx < ind->n_all_times && t == all_times[idx]){
	    par_ptr[op->par_cov[k]-1] = getValue(idx, y, ind);
	    if (idx == 0){
	      ind->cacheME=0;
	    } else if (getValue(idx, y, ind) != getValue(idx-1, y, ind)) {
	      ind->cacheME=0;
	    }
	  } else {
	    // Use the same methodology as approxfun.
	    ind->ylow = getValue(0, y, ind);/* cov_ptr[ind->n_all_times*k]; */
	    ind->yhigh = getValue(ind->n_all_times-1, y, ind);/* cov_ptr[ind->n_all_times*k+ind->n_all_times-1]; */
	    par_ptr[op->par_cov[k]-1] = rx_approxP(t, y, ind->n_all_times, op, ind);
	    // Don't need to reset ME because solver doesn't use the
	    // times in-between.
	  }
	}
      }
    }
  }
}

void doSort(rx_solving_options_ind *ind);
void calcMtime(int solveid, double *mtime);
// Advan-style linCmt solutions


/*
rxOptExpr(rxNorm(RxODE({
  A1=r1/ka-((r1+(-b1-A1last)*ka)*exp(-ka*t))/ka;
  A2=((r1+(-b1-A1last)*ka)*exp(-ka*t))/(ka-k20)-(((ka-k20)*r2+ka*r1+(-b2-b1-A2last-A1last)*k20*ka+(b2+A2last)*k20^2)*exp(-k20*t))/(k20*ka-k20^2)+(r2+r1)/k20}))) = 
*/

////////////////////////////////////////////////////////////////////////////////
// 1-3 oral absorption with rates
// From Richard Upton with RxODE Expression optimization (and some manual edits)
////////////////////////////////////////////////////////////////////////////////

#define A1 A[0]
#define A2 A[1]
#define A3 A[2]
#define A4 A[3]
#define A1last Alast[0]
#define A2last Alast[1]
#define A3last Alast[2]
#define A4last Alast[3]

static inline void oneCmtKaRateSSr1(double *A, double *r1,
				    double *ka, double *k20) {
  A1 = (*r1)/(*ka);
  A2 = (*r1)/(*k20);
}

static inline void oneCmtKaRateSSr2(double *A, double *r2,
				    double *ka, double *k20) {
  A1 = 0;
  A2 = (*r2)/(*k20);
}

static inline void oneCmtKaRateSStr1(double *A,
				     double *tinf, double *tau, double *r1,
				     double *ka, double *k20) {
  double eKa = exp(-(*ka)*((*tau)-(*tinf)))/(1.0-exp(-(*tau)*(*ka)));
  double eiKa = exp(-(*ka)*(*tinf));
  double eiK = exp(-(*k20)*(*tinf));
  double eK = exp(-(*k20)*((*tau)-(*tinf)))/(1.0-exp(-(*tau)*(*k20)));
  A1=eKa*((*r1)/(*ka) - eiKa*(*r1)/(*ka));
  A2=eK*((*r1)/(*k20) + eiKa*(*r1)/(-(*k20) + (*ka)) - eiK*(*r1)*(*ka)/((*ka)*(*k20) - (*k20)*(*k20))) + (*ka)*(eK - eKa)*((*r1)/(*ka) - eiKa*(*r1)/(*ka))/(-(*k20) + (*ka));
}

static inline void oneCmtKaRateSStr2(double *A,
				     double *tinf, double *tau, double *r2, 
				     double *ka, double *k20){
  double eiK = exp(-(*k20)*(*tinf));
  double eK = exp(-(*k20)*((*tau)-(*tinf)))/(1.0-exp(-(*k20)*(*tau)));
  A1=0.0;
  A2=eK*((*r2)/(*k20) - eiK*(*r2)*(-(*k20) + (*ka))/((*ka)*(*k20) - (*k20)*(*k20)));
}

static inline void oneCmtKaRate(double *A, double *Alast, double *t,
				double *b1, double *b2,
				double *r1, double *r2,
				double *ka, double *k20) {
  double eKa = exp(-(*ka)*(*t));
  double e20 = exp(-(*k20)*(*t));
  A1 = (*r1)/(*ka)-(((*r1)-A1last*(*ka))*eKa)/(*ka) + (*b1);
  A2 = (((*r1)-A1last*(*ka))*eKa)/((*ka)-(*k20)) - ((((*ka)-(*k20))*(*r2)+(*ka)*(*r1)+(-A2last-A1last)*(*k20)*(*ka)+A2last*(*k20)*(*k20))*e20)/((*k20)*(*ka)-(*k20)*(*k20))+ ((*r2)+(*r1))/(*k20) + (*b2);
}

/*
Two compartment with rates in each
 */

static inline void twoCmtKaRateSSr1(double *A, double *r1,
				    double *ka,  double *k20, 
				    double *k23, double *k32) {
  double s = (*k23)+(*k32)+(*k20);
  double beta  = 0.5*(s - sqrt(s*s - 4*(*k32)*(*k20)));
  double alpha = (*k32)*(*k20)/beta;
  A1=(*r1)/(*ka);
  A2=(*r1)*(*k32)/(beta*alpha);
  A3=(*r1)*(*k23)/(beta*alpha);
}

static inline void twoCmtKaRateSSr2(double *A, double *r2,
				    double *ka,  double *k20, 
				    double *k23, double *k32) {
  double s = (*k23)+(*k32)+(*k20);
  double beta  = 0.5*(s - sqrt(s*s - 4*(*k32)*(*k20)));
  double alpha = (*k32)*(*k20)/beta;
  A1=0;
  A2=(*r2)*(*k32)/(beta*alpha);
  A3=(*r2)*(*k23)/(beta*alpha);
}

static inline void twoCmtKaRateSStr1(double *A, double *tinf, double *tau, double *r1, 
				     double *ka,  double *k20, 
				     double *k23, double *k32){
  double s = (*k23)+(*k32)+(*k20);
  double beta  = 0.5*(s - sqrt(s*s - 4*(*k32)*(*k20)));
  double alpha = (*k32)*(*k20)/beta;

  double eA = exp(-alpha*((*tau)-(*tinf)))/(1.0-exp(-alpha*(*tau)));
  double eB = exp(-beta*((*tau)-(*tinf)))/(1.0-exp(-beta*(*tau)));

  double eiA = exp(-alpha*(*tinf));
  double eiB = exp(-beta*(*tinf));

  double alpha2 = alpha*alpha;
  double alpha3 = alpha2*alpha;

  double beta2 = beta*beta;
  double beta3 = beta2*beta;

  double ka2 = (*ka)*(*ka);

  double eKa = exp(-(*ka)*((*tau)-(*tinf)))/(1.0-exp(-(*ka)*(*tau)));
  double eiKa = exp(-(*ka)*(*tinf));

  A1=eKa*((*r1)/(*ka) - eiKa*(*r1)/(*ka));
  A2=(eA*(-alpha*((*r1)*(*k32)/(beta*alpha) + eiKa*(*r1)*(-(*k32) + (*ka))/(beta*alpha + (*ka)*(-alpha - beta) + ka2) - eiA*(*r1)*(*ka)*(-alpha + (*k32))/(-beta*alpha2 + (*ka)*(beta*alpha - alpha2) + alpha3) + eiB*(*r1)*(*ka)*(-beta + (*k32))/(beta2*alpha + (*ka)*(-beta*alpha + beta2) - beta3)) + (*k32)*((*r1)*(*k23)/(beta*alpha) - eiKa*(*r1)*(*k23)/(beta*alpha + (*ka)*(-alpha - beta) + ka2) - eiA*(*r1)*(*ka)*(*k23)/(-beta*alpha2 + (*ka)*(beta*alpha - alpha2) + alpha3) + eiB*(*r1)*(*ka)*(*k23)/(beta2*alpha + (*ka)*(-beta*alpha + beta2) - beta3)) + (*k32)*((*r1)*(*k32)/(beta*alpha) + eiKa*(*r1)*(-(*k32) + (*ka))/(beta*alpha + (*ka)*(-alpha - beta) + ka2) - eiA*(*r1)*(*ka)*(-alpha + (*k32))/(-beta*alpha2 + (*ka)*(beta*alpha - alpha2) + alpha3) + eiB*(*r1)*(*ka)*(-beta + (*k32))/(beta2*alpha + (*ka)*(-beta*alpha + beta2) - beta3))) - eB*(-beta*((*r1)*(*k32)/(beta*alpha) + eiKa*(*r1)*(-(*k32) + (*ka))/(beta*alpha + (*ka)*(-alpha - beta) + ka2) - eiA*(*r1)*(*ka)*(-alpha + (*k32))/(-beta*alpha2 + (*ka)*(beta*alpha - alpha2) + alpha3) + eiB*(*r1)*(*ka)*(-beta + (*k32))/(beta2*alpha + (*ka)*(-beta*alpha + beta2) - beta3)) + (*k32)*((*r1)*(*k23)/(beta*alpha) - eiKa*(*r1)*(*k23)/(beta*alpha + (*ka)*(-alpha - beta) + ka2) - eiA*(*r1)*(*ka)*(*k23)/(-beta*alpha2 + (*ka)*(beta*alpha - alpha2) + alpha3) + eiB*(*r1)*(*ka)*(*k23)/(beta2*alpha + (*ka)*(-beta*alpha + beta2) - beta3)) + (*k32)*((*r1)*(*k32)/(beta*alpha) + eiKa*(*r1)*(-(*k32) + (*ka))/(beta*alpha + (*ka)*(-alpha - beta) + ka2) - eiA*(*r1)*(*ka)*(-alpha + (*k32))/(-beta*alpha2 + (*ka)*(beta*alpha - alpha2) + alpha3) + eiB*(*r1)*(*ka)*(-beta + (*k32))/(beta2*alpha + (*ka)*(-beta*alpha + beta2) - beta3))))/(-alpha + beta) + (*ka)*(eA*(-alpha + (*k32))/((-alpha + beta)*(-alpha + (*ka))) + eB*(-beta + (*k32))/((-beta + (*ka))*(alpha - beta)) + eKa*((*k32) - (*ka))/((beta - (*ka))*(alpha - (*ka))))*((*r1)/(*ka) - eiKa*(*r1)/(*ka));
  A3=(eA*(-alpha*((*r1)*(*k23)/(beta*alpha) - eiKa*(*r1)*(*k23)/(beta*alpha + (*ka)*(-alpha - beta) + ka2) - eiA*(*r1)*(*ka)*(*k23)/(-beta*alpha2 + (*ka)*(beta*alpha - alpha2) + alpha3) + eiB*(*r1)*(*ka)*(*k23)/(beta2*alpha + (*ka)*(-beta*alpha + beta2) - beta3)) + (*k23)*((*r1)*(*k32)/(beta*alpha) + eiKa*(*r1)*(-(*k32) + (*ka))/(beta*alpha + (*ka)*(-alpha - beta) + ka2) - eiA*(*r1)*(*ka)*(-alpha + (*k32))/(-beta*alpha2 + (*ka)*(beta*alpha - alpha2) + alpha3) + eiB*(*r1)*(*ka)*(-beta + (*k32))/(beta2*alpha + (*ka)*(-beta*alpha + beta2) - beta3)) + ((*k20) + (*k23))*((*r1)*(*k23)/(beta*alpha) - eiKa*(*r1)*(*k23)/(beta*alpha + (*ka)*(-alpha - beta) + ka2) - eiA*(*r1)*(*ka)*(*k23)/(-beta*alpha2 + (*ka)*(beta*alpha - alpha2) + alpha3) + eiB*(*r1)*(*ka)*(*k23)/(beta2*alpha + (*ka)*(-beta*alpha + beta2) - beta3))) - eB*(-beta*((*r1)*(*k23)/(beta*alpha) - eiKa*(*r1)*(*k23)/(beta*alpha + (*ka)*(-alpha - beta) + ka2) - eiA*(*r1)*(*ka)*(*k23)/(-beta*alpha2 + (*ka)*(beta*alpha - alpha2) + alpha3) + eiB*(*r1)*(*ka)*(*k23)/(beta2*alpha + (*ka)*(-beta*alpha + beta2) - beta3)) + (*k23)*((*r1)*(*k32)/(beta*alpha) + eiKa*(*r1)*(-(*k32) + (*ka))/(beta*alpha + (*ka)*(-alpha - beta) + ka2) - eiA*(*r1)*(*ka)*(-alpha + (*k32))/(-beta*alpha2 + (*ka)*(beta*alpha - alpha2) + alpha3) + eiB*(*r1)*(*ka)*(-beta + (*k32))/(beta2*alpha + (*ka)*(-beta*alpha + beta2) - beta3)) + ((*k20) + (*k23))*((*r1)*(*k23)/(beta*alpha) - eiKa*(*r1)*(*k23)/(beta*alpha + (*ka)*(-alpha - beta) + ka2) - eiA*(*r1)*(*ka)*(*k23)/(-beta*alpha2 + (*ka)*(beta*alpha - alpha2) + alpha3) + eiB*(*r1)*(*ka)*(*k23)/(beta2*alpha + (*ka)*(-beta*alpha + beta2) - beta3))))/(-alpha + beta) + (*ka)*(*k23)*(eA/((-alpha + beta)*(-alpha + (*ka))) + eB/((-beta + (*ka))*(alpha - beta)) + eKa/((beta - (*ka))*(alpha - (*ka))))*((*r1)/(*ka) - eiKa*(*r1)/(*ka));
}

static inline void twoCmtKaRateSStr2(double *A, double *tinf, double *tau, double *r2,
				     double *ka,  double *k20, 
				     double *k23, double *k32) {
  double E2 = (*k20)+(*k23);
  double E3 = (*k32);
  double s = (*k23)+(*k32)+(*k20);
  double beta  = 0.5*(s - sqrt(s*s - 4*(*k32)*(*k20)));
  double alpha = (*k32)*(*k20)/beta;

  double eA = exp(-alpha*((*tau)-(*tinf)))/(1.0-exp(-alpha*(*tau)));
  double eB = exp(-beta*((*tau)-(*tinf)))/(1.0-exp(-beta*(*tau)));

  double eiA = exp(-alpha*(*tinf));
  double eiB = exp(-beta*(*tinf));

  double alpha2 = alpha*alpha;
  double alpha3 = alpha2*alpha;

  double beta2 = beta*beta;
  double beta3 = beta2*beta;
  A1=0.0;
  A2=(eA*(E3*((*r2)*(*k32)/(beta*alpha) - eiA*(*r2)*(-(*k32)*alpha + (*ka)*(-alpha + (*k32)) + alpha2)/(-beta*alpha2 + (*ka)*(beta*alpha - alpha2) + alpha3) + eiB*(*r2)*(-(*k32)*beta + (*ka)*(-beta + (*k32)) + beta2)/(beta2*alpha + (*ka)*(-beta*alpha + beta2) - beta3)) - alpha*((*r2)*(*k32)/(beta*alpha) - eiA*(*r2)*(-(*k32)*alpha + (*ka)*(-alpha + (*k32)) + alpha2)/(-beta*alpha2 + (*ka)*(beta*alpha - alpha2) + alpha3) + eiB*(*r2)*(-(*k32)*beta + (*ka)*(-beta + (*k32)) + beta2)/(beta2*alpha + (*ka)*(-beta*alpha + beta2) - beta3)) + (*k32)*((*r2)*(*k23)/(beta*alpha) - eiA*(*r2)*(-(*k23)*alpha + (*ka)*(*k23))/(-beta*alpha2 + (*ka)*(beta*alpha - alpha2) + alpha3) + eiB*(*r2)*(-(*k23)*beta + (*ka)*(*k23))/(beta2*alpha + (*ka)*(-beta*alpha + beta2) - beta3))) - eB*(E3*((*r2)*(*k32)/(beta*alpha) - eiA*(*r2)*(-(*k32)*alpha + (*ka)*(-alpha + (*k32)) + alpha2)/(-beta*alpha2 + (*ka)*(beta*alpha - alpha2) + alpha3) + eiB*(*r2)*(-(*k32)*beta + (*ka)*(-beta + (*k32)) + beta2)/(beta2*alpha + (*ka)*(-beta*alpha + beta2) - beta3)) - beta*((*r2)*(*k32)/(beta*alpha) - eiA*(*r2)*(-(*k32)*alpha + (*ka)*(-alpha + (*k32)) + alpha2)/(-beta*alpha2 + (*ka)*(beta*alpha - alpha2) + alpha3) + eiB*(*r2)*(-(*k32)*beta + (*ka)*(-beta + (*k32)) + beta2)/(beta2*alpha + (*ka)*(-beta*alpha + beta2) - beta3)) + (*k32)*((*r2)*(*k23)/(beta*alpha) - eiA*(*r2)*(-(*k23)*alpha + (*ka)*(*k23))/(-beta*alpha2 + (*ka)*(beta*alpha - alpha2) + alpha3) + eiB*(*r2)*(-(*k23)*beta + (*ka)*(*k23))/(beta2*alpha + (*ka)*(-beta*alpha + beta2) - beta3))))/(-alpha + beta);
  A3=(eA*(E2*((*r2)*(*k23)/(beta*alpha) - eiA*(*r2)*(-(*k23)*alpha + (*ka)*(*k23))/(-beta*alpha2 + (*ka)*(beta*alpha - alpha2) + alpha3) + eiB*(*r2)*(-(*k23)*beta + (*ka)*(*k23))/(beta2*alpha + (*ka)*(-beta*alpha + beta2) - beta3)) - alpha*((*r2)*(*k23)/(beta*alpha) - eiA*(*r2)*(-(*k23)*alpha + (*ka)*(*k23))/(-beta*alpha2 + (*ka)*(beta*alpha - alpha2) + alpha3) + eiB*(*r2)*(-(*k23)*beta + (*ka)*(*k23))/(beta2*alpha + (*ka)*(-beta*alpha + beta2) - beta3)) + (*k23)*((*r2)*(*k32)/(beta*alpha) - eiA*(*r2)*(-(*k32)*alpha + (*ka)*(-alpha + (*k32)) + alpha2)/(-beta*alpha2 + (*ka)*(beta*alpha - alpha2) + alpha3) + eiB*(*r2)*(-(*k32)*beta + (*ka)*(-beta + (*k32)) + beta2)/(beta2*alpha + (*ka)*(-beta*alpha + beta2) - beta3))) - eB*(E2*((*r2)*(*k23)/(beta*alpha) - eiA*(*r2)*(-(*k23)*alpha + (*ka)*(*k23))/(-beta*alpha2 + (*ka)*(beta*alpha - alpha2) + alpha3) + eiB*(*r2)*(-(*k23)*beta + (*ka)*(*k23))/(beta2*alpha + (*ka)*(-beta*alpha + beta2) - beta3)) - beta*((*r2)*(*k23)/(beta*alpha) - eiA*(*r2)*(-(*k23)*alpha + (*ka)*(*k23))/(-beta*alpha2 + (*ka)*(beta*alpha - alpha2) + alpha3) + eiB*(*r2)*(-(*k23)*beta + (*ka)*(*k23))/(beta2*alpha + (*ka)*(-beta*alpha + beta2) - beta3)) + (*k23)*((*r2)*(*k32)/(beta*alpha) - eiA*(*r2)*(-(*k32)*alpha + (*ka)*(-alpha + (*k32)) + alpha2)/(-beta*alpha2 + (*ka)*(beta*alpha - alpha2) + alpha3) + eiB*(*r2)*(-(*k32)*beta + (*ka)*(-beta + (*k32)) + beta2)/(beta2*alpha + (*ka)*(-beta*alpha + beta2) - beta3))))/(-alpha + beta);
}

static inline void twoCmtKaRate(double *A, double *Alast,
				double *t, double *b1, double *b2,
				double *r1, double *r2,
				double *ka,  double *k20, 
				double *k23, double *k32) {
  double E2 =  (*k20)+ (*k23);
  double s = (*k23)+(*k32)+(*k20);
  double beta  = 0.5*(s - sqrt(s*s - 4*(*k32)*(*k20)));
  double alpha = (*k32)*(*k20)/beta;

  double eKa = exp(-(*ka)*(*t));
  double eA = exp(-alpha*(*t));
  double eB = exp(-beta*(*t));

  double ka2 = (*ka)*(*ka);
  
  double alpha2 = alpha*alpha;
  double alpha3 = alpha2*alpha;
  
  double beta2 = beta*beta;
  double beta3 = beta2*beta;
  
  A1 = (*b1)+(*r1)/(*ka)-(((*r1)-A1last*(*ka))*eKa)/(*ka);
  A2 = (*b2)+((((*ka)-(*k32))*(*r1)-A1last*ka2+A1last*(*k32)*(*ka))*eKa)/(ka2+(-beta-alpha)*(*ka)+alpha*beta)+(((((*k32)-beta)*(*ka)-beta*(*k32)+beta2)*(*r2)+((*k32)-beta)*(*ka)*(*r1)+((-A3last-A2last-A1last)*beta*(*k32)+(A2last+A1last)*beta2)*(*ka)+(A3last+A2last)*beta2*(*k32)-A2last*beta3)*eB)/((beta2-alpha*beta)*(*ka)-beta3+alpha*beta2)-(((((*k32)-alpha)*(*ka)-alpha*(*k32)+alpha2)*(*r2)+((*k32)-alpha)*(*ka)*(*r1)+((-A3last-A2last-A1last)*alpha*(*k32)+(A2last+A1last)*alpha2)*(*ka)+(A3last+A2last)*alpha2*(*k32)-A2last*alpha3)*eA)/((alpha*beta-alpha2)*(*ka)-alpha2*beta+alpha3)+((*k32)*(*r2)+(*k32)*(*r1))/(alpha*beta);
  A3 = -(((*k23)*(*r1)-A1last*(*k23)*(*ka))*eKa)/(ka2+(-beta-alpha)*(*ka)+alpha*beta)+((((*k23)*(*ka)-beta*(*k23))*(*r2)+(*k23)*(*ka)*(*r1)+((-A2last-A1last)*beta*(*k23)+A3last*beta2-A3last*E2*beta)*(*ka)+A2last*beta2*(*k23)-A3last*beta3+A3last*E2*beta2)*eB)/((beta2-alpha*beta)*(*ka)-beta3+alpha*beta2)-((((*k23)*(*ka)-alpha*(*k23))*(*r2)+(*k23)*(*ka)*(*r1)+((-A2last-A1last)*alpha*(*k23)+A3last*alpha2-A3last*E2*alpha)*(*ka)+A2last*alpha2*(*k23)-A3last*alpha3+A3last*E2*alpha2)*eA)/((alpha*beta-alpha2)*(*ka)-alpha2*beta+alpha3)+((*k23)*(*r2)+(*k23)*(*r1))/(alpha*beta);
}

static inline void threeCmtKaRateSSr1(double *A, double *r1,
				      double *ka, double *k20,
				      double *k23, double *k32,
				      double *k24, double *k42){
  double j = (*k23)+(*k20)+(*k32)+(*k42)+(*k24);
  double k = (*k23)*(*k42)+(*k20)*(*k32)+(*k20)*(*k42)+(*k32)*(*k42)+(*k24)*(*k32);
  double l = (*k20)*(*k32)*(*k42);

  double m = 0.3333333333333333*(3.0*k - j*j);
  double n = 0.03703703703703703*(2.0*j*j*j - 9.0*j*k + 27.0*l);
  double Q = 0.25*n*n + 0.03703703703703703*m*m*m;

  double alpha = sqrt(-Q);
  double beta = -0.5*n;
  double rho=sqrt(beta*beta+alpha*alpha);
  double theta = atan2(alpha,beta);
  double ct3 = cos(0.3333333333333333*theta);
  double rho3 = R_pow(rho,0.3333333333333333);
  double st3 = 1.732050807568877193177*sin(0.3333333333333333*theta);
  double j3 = 0.3333333333333333*j;
  double lam1 = j3  + rho3*(ct3 + st3);
  double lam2 = j3 + rho3*(ct3 - st3);
  double lam3 = j3 -(2.0*rho3*ct3);
  double l123 = 1.0/(lam1*lam2*lam3);
  A1=(*r1)/(*ka);
  A2=(*r1)*(*k42)*(*k32)*l123;
  A3=(*r1)*(*k42)*(*k23)*l123;
  A4=(*r1)*(*k24)*(*k32)*l123;
}

static inline void threeCmtKaRateSSr2(double *A, double *r2, 
				      double *ka, double *k20,
				      double *k23, double *k32,
				      double *k24, double *k42){
  double j = (*k23)+(*k20)+(*k32)+(*k42)+(*k24);
  double k = (*k23)*(*k42)+(*k20)*(*k32)+(*k20)*(*k42)+(*k32)*(*k42)+(*k24)*(*k32);
  double l = (*k20)*(*k32)*(*k42);

  double m = 0.3333333333333333*(3.0*k - j*j);
  double n = 0.03703703703703703*(2.0*j*j*j - 9.0*j*k + 27.0*l);
  double Q = 0.25*n*n + 0.03703703703703703*m*m*m;

  double alpha = sqrt(-Q);
  double beta = -0.5*n;
  double rho=sqrt(beta*beta+alpha*alpha);
  double theta = atan2(alpha,beta);
  double ct3 = cos(0.3333333333333333*theta);
  double rho3 = R_pow(rho,0.3333333333333333);
  double st3 = 1.732050807568877193177*sin(0.3333333333333333*theta);
  double j3 = 0.3333333333333333*j;
  double lam1 = j3  + rho3*(ct3 + st3);
  double lam2 = j3 + rho3*(ct3 - st3);
  double lam3 = j3 -(2.0*rho3*ct3);
  double l123 = 1.0/(lam1*lam2*lam3);
  A1=0;
  A2=(*r2)*(*k42)*(*k32)*l123;
  A3=(*r2)*(*k42)*(*k23)*l123;
  A4=(*r2)*(*k24)*(*k32)*l123;
}

static inline void threeCmtKaRateSStr1(double *A, double *tinf, double *tau, double *r1, 
				       double *ka, double *k20,
				       double *k23, double *k32,
				       double *k24, double *k42){
  double E2 =  (*k20)+ (*k23) + (*k24);
  double E3 = (*k32);
  double E4 = (*k42);
  double j = (*k23)+(*k20)+(*k32)+(*k42)+(*k24);
  double k = (*k23)*(*k42)+(*k20)*(*k32)+(*k20)*(*k42)+(*k32)*(*k42)+(*k24)*(*k32);
  double l = (*k20)*(*k32)*(*k42);

  double m = 0.3333333333333333*(3.0*k - j*j);
  double n = 0.03703703703703703*(2.0*j*j*j - 9.0*j*k + 27.0*l);
  double Q = 0.25*n*n + 0.03703703703703703*m*m*m;

  double alpha = sqrt(-Q);
  double beta = -0.5*n;
  double rho=sqrt(beta*beta+alpha*alpha);
  double theta = atan2(alpha,beta);
  double ct3 = cos(0.3333333333333333*theta);
  double rho3 = R_pow(rho,0.3333333333333333);
  double st3 = 1.732050807568877193177*sin(0.3333333333333333*theta);
  double j3 = 0.3333333333333333*j;
  double lam1 = j3  + rho3*(ct3 + st3);
  double lam2 = j3 + rho3*(ct3 - st3);
  double lam3 = j3 -(2.0*rho3*ct3);
  
  double eKa = exp(-(*ka)*((*tau)-(*tinf)))/(1.0-exp(-(*ka)*(*tau)));
  double eiKa = exp(-(*ka)*(*tinf));

  double eL1 = exp(-lam1*((*tau)-(*tinf)))/(1.0-exp(-lam1*(*tau)));
  double eiL1 = exp(-lam1*(*tinf));

  double eL2 = exp(-lam2*((*tau)-(*tinf)))/(1.0-exp(-lam2*(*tau)));
  double eiL2 = exp(-lam2*(*tinf));

  double eL3 = exp(-lam3*((*tau)-(*tinf)))/(1.0-exp(-lam3*(*tau)));
  double eiL3 = exp(-lam3*(*tinf));

  double ka2 = (*ka)*(*ka);
  double ka3 = ka2*(*ka);

  double lam12 = lam1*lam1;
  double lam13 = lam12*lam1;
  double lam14 = lam13*lam1;

  double lam22 = lam2*lam2;
  double lam23 = lam22*lam2;
  double lam24 = lam23*lam2;

  double lam32 = lam3*lam3;
  double lam33 = lam32*lam3;
  double lam34 = lam33*lam3;
  A1=eKa*((*r1)/(*ka) - eiKa*(*r1)/(*ka));
  A2=(eL1*(E4 - lam1)*(E3 - lam1)/((-lam1 + lam3)*(-lam1 + lam2)) + eL2*(E4 - lam2)*(E3 - lam2)/((-lam2 + lam3)*(lam1 - lam2)) + eL3*(E3 - lam3)*(E4 - lam3)/((lam2 - lam3)*(lam1 - lam3)))*(-eiKa*(*r1)*((*k42)*(*k32) + (-(*k32) - (*k42))*(*ka) + ka2)/(ka2*lam1 + lam2*(-(*ka)*lam1 + ka2) + lam3*(-(*ka)*lam1 + lam2*(-(*ka) + lam1) + ka2) - ka3) - eiL1*(*r1)*(-(*ka)*lam12 - (*ka)*(*k42)*(*k32) + ((*k32) + (*k42))*(*ka)*lam1)/(-(*ka)*lam13 + lam2*((*ka)*lam12 - lam13) + lam3*((*ka)*lam12 + lam2*(-(*ka)*lam1 + lam12) - lam13) + lam14) + eiL2*(*r1)*(-(*ka)*lam22 - (*ka)*(*k42)*(*k32) + ((*k32) + (*k42))*(*ka)*lam2)/(lam23*((*ka) + lam1) + lam3*(lam22*(-(*ka) - lam1) + (*ka)*lam2*lam1 + lam23) - (*ka)*lam22*lam1 - lam24) - eiL3*(*r1)*(-(*ka)*lam32 - (*ka)*(*k42)*(*k32) + ((*k32) + (*k42))*(*ka)*lam3)/(lam32*((*ka)*lam1 + lam2*((*ka) + lam1)) + (-(*ka) - lam1 - lam2)*lam33 - (*ka)*lam2*lam1*lam3 + lam34) + (*r1)*(*k42)*(*k32)/(lam2*lam1*lam3)) + (*ka)*(eL1*(E4 - lam1)*(E3 - lam1)/((-lam1 + lam3)*(-lam1 + lam2)*((*ka) - lam1)) + eL2*(E4 - lam2)*(E3 - lam2)/((-lam2 + lam3)*(lam1 - lam2)*((*ka) - lam2)) + eL3*(E3 - lam3)*(E4 - lam3)/((lam2 - lam3)*(lam1 - lam3)*((*ka) - lam3)) + eKa*(E3 - (*ka))*(E4 - (*ka))/((-(*ka) + lam1)*(-(*ka) + lam2)*(-(*ka) + lam3)))*((*r1)/(*ka) - eiKa*(*r1)/(*ka)) + eL1*(-lam1*((*k32)*(eiKa*(*r1)*(-(*k42)*(*k23) + (*ka)*(*k23))/(ka2*lam1 + lam2*(-(*ka)*lam1 + ka2) + lam3*(-(*ka)*lam1 + lam2*(-(*ka) + lam1) + ka2) - ka3) + eiL1*(*r1)*(-(*ka)*(*k23)*lam1 + (*ka)*(*k42)*(*k23))/(-(*ka)*lam13 + lam2*((*ka)*lam12 - lam13) + lam3*((*ka)*lam12 + lam2*(-(*ka)*lam1 + lam12) - lam13) + lam14) - eiL2*(*r1)*(-(*ka)*(*k23)*lam2 + (*ka)*(*k42)*(*k23))/(lam23*((*ka) + lam1) + lam3*(lam22*(-(*ka) - lam1) + (*ka)*lam2*lam1 + lam23) - (*ka)*lam22*lam1 - lam24) + eiL3*(*r1)*(-(*ka)*(*k23)*lam3 + (*ka)*(*k42)*(*k23))/(lam32*((*ka)*lam1 + lam2*((*ka) + lam1)) + (-(*ka) - lam1 - lam2)*lam33 - (*ka)*lam2*lam1*lam3 + lam34) + (*r1)*(*k42)*(*k23)/(lam2*lam1*lam3)) + (*k42)*(eiKa*(*r1)*(-(*k24)*(*k32) + (*ka)*(*k24))/(ka2*lam1 + lam2*(-(*ka)*lam1 + ka2) + lam3*(-(*ka)*lam1 + lam2*(-(*ka) + lam1) + ka2) - ka3) + eiL1*(*r1)*((*ka)*(*k24)*(*k32) - (*ka)*(*k24)*lam1)/(-(*ka)*lam13 + lam2*((*ka)*lam12 - lam13) + lam3*((*ka)*lam12 + lam2*(-(*ka)*lam1 + lam12) - lam13) + lam14) - eiL2*(*r1)*((*ka)*(*k24)*(*k32) - (*ka)*(*k24)*lam2)/(lam23*((*ka) + lam1) + lam3*(lam22*(-(*ka) - lam1) + (*ka)*lam2*lam1 + lam23) - (*ka)*lam22*lam1 - lam24) + eiL3*(*r1)*((*ka)*(*k24)*(*k32) - (*ka)*(*k24)*lam3)/(lam32*((*ka)*lam1 + lam2*((*ka) + lam1)) + (-(*ka) - lam1 - lam2)*lam33 - (*ka)*lam2*lam1*lam3 + lam34) + (*r1)*(*k24)*(*k32)/(lam2*lam1*lam3))) + E3*(*k42)*(eiKa*(*r1)*(-(*k24)*(*k32) + (*ka)*(*k24))/(ka2*lam1 + lam2*(-(*ka)*lam1 + ka2) + lam3*(-(*ka)*lam1 + lam2*(-(*ka) + lam1) + ka2) - ka3) + eiL1*(*r1)*((*ka)*(*k24)*(*k32) - (*ka)*(*k24)*lam1)/(-(*ka)*lam13 + lam2*((*ka)*lam12 - lam13) + lam3*((*ka)*lam12 + lam2*(-(*ka)*lam1 + lam12) - lam13) + lam14) - eiL2*(*r1)*((*ka)*(*k24)*(*k32) - (*ka)*(*k24)*lam2)/(lam23*((*ka) + lam1) + lam3*(lam22*(-(*ka) - lam1) + (*ka)*lam2*lam1 + lam23) - (*ka)*lam22*lam1 - lam24) + eiL3*(*r1)*((*ka)*(*k24)*(*k32) - (*ka)*(*k24)*lam3)/(lam32*((*ka)*lam1 + lam2*((*ka) + lam1)) + (-(*ka) - lam1 - lam2)*lam33 - (*ka)*lam2*lam1*lam3 + lam34) + (*r1)*(*k24)*(*k32)/(lam2*lam1*lam3)) + E4*(*k32)*(eiKa*(*r1)*(-(*k42)*(*k23) + (*ka)*(*k23))/(ka2*lam1 + lam2*(-(*ka)*lam1 + ka2) + lam3*(-(*ka)*lam1 + lam2*(-(*ka) + lam1) + ka2) - ka3) + eiL1*(*r1)*(-(*ka)*(*k23)*lam1 + (*ka)*(*k42)*(*k23))/(-(*ka)*lam13 + lam2*((*ka)*lam12 - lam13) + lam3*((*ka)*lam12 + lam2*(-(*ka)*lam1 + lam12) - lam13) + lam14) - eiL2*(*r1)*(-(*ka)*(*k23)*lam2 + (*ka)*(*k42)*(*k23))/(lam23*((*ka) + lam1) + lam3*(lam22*(-(*ka) - lam1) + (*ka)*lam2*lam1 + lam23) - (*ka)*lam22*lam1 - lam24) + eiL3*(*r1)*(-(*ka)*(*k23)*lam3 + (*ka)*(*k42)*(*k23))/(lam32*((*ka)*lam1 + lam2*((*ka) + lam1)) + (-(*ka) - lam1 - lam2)*lam33 - (*ka)*lam2*lam1*lam3 + lam34) + (*r1)*(*k42)*(*k23)/(lam2*lam1*lam3)))/((lam1 - lam3)*(lam1 - lam2)) + eL3*(lam3*((*k32)*(eiKa*(*r1)*(-(*k42)*(*k23) + (*ka)*(*k23))/(ka2*lam1 + lam2*(-(*ka)*lam1 + ka2) + lam3*(-(*ka)*lam1 + lam2*(-(*ka) + lam1) + ka2) - ka3) + eiL1*(*r1)*(-(*ka)*(*k23)*lam1 + (*ka)*(*k42)*(*k23))/(-(*ka)*lam13 + lam2*((*ka)*lam12 - lam13) + lam3*((*ka)*lam12 + lam2*(-(*ka)*lam1 + lam12) - lam13) + lam14) - eiL2*(*r1)*(-(*ka)*(*k23)*lam2 + (*ka)*(*k42)*(*k23))/(lam23*((*ka) + lam1) + lam3*(lam22*(-(*ka) - lam1) + (*ka)*lam2*lam1 + lam23) - (*ka)*lam22*lam1 - lam24) + eiL3*(*r1)*(-(*ka)*(*k23)*lam3 + (*ka)*(*k42)*(*k23))/(lam32*((*ka)*lam1 + lam2*((*ka) + lam1)) + (-(*ka) - lam1 - lam2)*lam33 - (*ka)*lam2*lam1*lam3 + lam34) + (*r1)*(*k42)*(*k23)/(lam2*lam1*lam3)) + (*k42)*(eiKa*(*r1)*(-(*k24)*(*k32) + (*ka)*(*k24))/(ka2*lam1 + lam2*(-(*ka)*lam1 + ka2) + lam3*(-(*ka)*lam1 + lam2*(-(*ka) + lam1) + ka2) - ka3) + eiL1*(*r1)*((*ka)*(*k24)*(*k32) - (*ka)*(*k24)*lam1)/(-(*ka)*lam13 + lam2*((*ka)*lam12 - lam13) + lam3*((*ka)*lam12 + lam2*(-(*ka)*lam1 + lam12) - lam13) + lam14) - eiL2*(*r1)*((*ka)*(*k24)*(*k32) - (*ka)*(*k24)*lam2)/(lam23*((*ka) + lam1) + lam3*(lam22*(-(*ka) - lam1) + (*ka)*lam2*lam1 + lam23) - (*ka)*lam22*lam1 - lam24) + eiL3*(*r1)*((*ka)*(*k24)*(*k32) - (*ka)*(*k24)*lam3)/(lam32*((*ka)*lam1 + lam2*((*ka) + lam1)) + (-(*ka) - lam1 - lam2)*lam33 - (*ka)*lam2*lam1*lam3 + lam34) + (*r1)*(*k24)*(*k32)/(lam2*lam1*lam3))) - (E3*(*k42)*(eiKa*(*r1)*(-(*k24)*(*k32) + (*ka)*(*k24))/(ka2*lam1 + lam2*(-(*ka)*lam1 + ka2) + lam3*(-(*ka)*lam1 + lam2*(-(*ka) + lam1) + ka2) - ka3) + eiL1*(*r1)*((*ka)*(*k24)*(*k32) - (*ka)*(*k24)*lam1)/(-(*ka)*lam13 + lam2*((*ka)*lam12 - lam13) + lam3*((*ka)*lam12 + lam2*(-(*ka)*lam1 + lam12) - lam13) + lam14) - eiL2*(*r1)*((*ka)*(*k24)*(*k32) - (*ka)*(*k24)*lam2)/(lam23*((*ka) + lam1) + lam3*(lam22*(-(*ka) - lam1) + (*ka)*lam2*lam1 + lam23) - (*ka)*lam22*lam1 - lam24) + eiL3*(*r1)*((*ka)*(*k24)*(*k32) - (*ka)*(*k24)*lam3)/(lam32*((*ka)*lam1 + lam2*((*ka) + lam1)) + (-(*ka) - lam1 - lam2)*lam33 - (*ka)*lam2*lam1*lam3 + lam34) + (*r1)*(*k24)*(*k32)/(lam2*lam1*lam3)) + E4*(*k32)*(eiKa*(*r1)*(-(*k42)*(*k23) + (*ka)*(*k23))/(ka2*lam1 + lam2*(-(*ka)*lam1 + ka2) + lam3*(-(*ka)*lam1 + lam2*(-(*ka) + lam1) + ka2) - ka3) + eiL1*(*r1)*(-(*ka)*(*k23)*lam1 + (*ka)*(*k42)*(*k23))/(-(*ka)*lam13 + lam2*((*ka)*lam12 - lam13) + lam3*((*ka)*lam12 + lam2*(-(*ka)*lam1 + lam12) - lam13) + lam14) - eiL2*(*r1)*(-(*ka)*(*k23)*lam2 + (*ka)*(*k42)*(*k23))/(lam23*((*ka) + lam1) + lam3*(lam22*(-(*ka) - lam1) + (*ka)*lam2*lam1 + lam23) - (*ka)*lam22*lam1 - lam24) + eiL3*(*r1)*(-(*ka)*(*k23)*lam3 + (*ka)*(*k42)*(*k23))/(lam32*((*ka)*lam1 + lam2*((*ka) + lam1)) + (-(*ka) - lam1 - lam2)*lam33 - (*ka)*lam2*lam1*lam3 + lam34) + (*r1)*(*k42)*(*k23)/(lam2*lam1*lam3))))/((-lam2 + lam3)*(lam1 - lam3)) + eL2*(lam2*((*k32)*(eiKa*(*r1)*(-(*k42)*(*k23) + (*ka)*(*k23))/(ka2*lam1 + lam2*(-(*ka)*lam1 + ka2) + lam3*(-(*ka)*lam1 + lam2*(-(*ka) + lam1) + ka2) - ka3) + eiL1*(*r1)*(-(*ka)*(*k23)*lam1 + (*ka)*(*k42)*(*k23))/(-(*ka)*lam13 + lam2*((*ka)*lam12 - lam13) + lam3*((*ka)*lam12 + lam2*(-(*ka)*lam1 + lam12) - lam13) + lam14) - eiL2*(*r1)*(-(*ka)*(*k23)*lam2 + (*ka)*(*k42)*(*k23))/(lam23*((*ka) + lam1) + lam3*(lam22*(-(*ka) - lam1) + (*ka)*lam2*lam1 + lam23) - (*ka)*lam22*lam1 - lam24) + eiL3*(*r1)*(-(*ka)*(*k23)*lam3 + (*ka)*(*k42)*(*k23))/(lam32*((*ka)*lam1 + lam2*((*ka) + lam1)) + (-(*ka) - lam1 - lam2)*lam33 - (*ka)*lam2*lam1*lam3 + lam34) + (*r1)*(*k42)*(*k23)/(lam2*lam1*lam3)) + (*k42)*(eiKa*(*r1)*(-(*k24)*(*k32) + (*ka)*(*k24))/(ka2*lam1 + lam2*(-(*ka)*lam1 + ka2) + lam3*(-(*ka)*lam1 + lam2*(-(*ka) + lam1) + ka2) - ka3) + eiL1*(*r1)*((*ka)*(*k24)*(*k32) - (*ka)*(*k24)*lam1)/(-(*ka)*lam13 + lam2*((*ka)*lam12 - lam13) + lam3*((*ka)*lam12 + lam2*(-(*ka)*lam1 + lam12) - lam13) + lam14) - eiL2*(*r1)*((*ka)*(*k24)*(*k32) - (*ka)*(*k24)*lam2)/(lam23*((*ka) + lam1) + lam3*(lam22*(-(*ka) - lam1) + (*ka)*lam2*lam1 + lam23) - (*ka)*lam22*lam1 - lam24) + eiL3*(*r1)*((*ka)*(*k24)*(*k32) - (*ka)*(*k24)*lam3)/(lam32*((*ka)*lam1 + lam2*((*ka) + lam1)) + (-(*ka) - lam1 - lam2)*lam33 - (*ka)*lam2*lam1*lam3 + lam34) + (*r1)*(*k24)*(*k32)/(lam2*lam1*lam3))) - (E3*(*k42)*(eiKa*(*r1)*(-(*k24)*(*k32) + (*ka)*(*k24))/(ka2*lam1 + lam2*(-(*ka)*lam1 + ka2) + lam3*(-(*ka)*lam1 + lam2*(-(*ka) + lam1) + ka2) - ka3) + eiL1*(*r1)*((*ka)*(*k24)*(*k32) - (*ka)*(*k24)*lam1)/(-(*ka)*lam13 + lam2*((*ka)*lam12 - lam13) + lam3*((*ka)*lam12 + lam2*(-(*ka)*lam1 + lam12) - lam13) + lam14) - eiL2*(*r1)*((*ka)*(*k24)*(*k32) - (*ka)*(*k24)*lam2)/(lam23*((*ka) + lam1) + lam3*(lam22*(-(*ka) - lam1) + (*ka)*lam2*lam1 + lam23) - (*ka)*lam22*lam1 - lam24) + eiL3*(*r1)*((*ka)*(*k24)*(*k32) - (*ka)*(*k24)*lam3)/(lam32*((*ka)*lam1 + lam2*((*ka) + lam1)) + (-(*ka) - lam1 - lam2)*lam33 - (*ka)*lam2*lam1*lam3 + lam34) + (*r1)*(*k24)*(*k32)/(lam2*lam1*lam3)) + E4*(*k32)*(eiKa*(*r1)*(-(*k42)*(*k23) + (*ka)*(*k23))/(ka2*lam1 + lam2*(-(*ka)*lam1 + ka2) + lam3*(-(*ka)*lam1 + lam2*(-(*ka) + lam1) + ka2) - ka3) + eiL1*(*r1)*(-(*ka)*(*k23)*lam1 + (*ka)*(*k42)*(*k23))/(-(*ka)*lam13 + lam2*((*ka)*lam12 - lam13) + lam3*((*ka)*lam12 + lam2*(-(*ka)*lam1 + lam12) - lam13) + lam14) - eiL2*(*r1)*(-(*ka)*(*k23)*lam2 + (*ka)*(*k42)*(*k23))/(lam23*((*ka) + lam1) + lam3*(lam22*(-(*ka) - lam1) + (*ka)*lam2*lam1 + lam23) - (*ka)*lam22*lam1 - lam24) + eiL3*(*r1)*(-(*ka)*(*k23)*lam3 + (*ka)*(*k42)*(*k23))/(lam32*((*ka)*lam1 + lam2*((*ka) + lam1)) + (-(*ka) - lam1 - lam2)*lam33 - (*ka)*lam2*lam1*lam3 + lam34) + (*r1)*(*k42)*(*k23)/(lam2*lam1*lam3))))/((lam2 - lam3)*(lam1 - lam2));
  A3=(eL1*(E4 - lam1)*(E2 - lam1)/((-lam1 + lam3)*(-lam1 + lam2)) + eL2*(E4 - lam2)*(E2 - lam2)/((-lam2 + lam3)*(lam1 - lam2)) + eL3*(E2 - lam3)*(E4 - lam3)/((lam2 - lam3)*(lam1 - lam3)))*(eiKa*(*r1)*(-(*k42)*(*k23) + (*ka)*(*k23))/(ka2*lam1 + lam2*(-(*ka)*lam1 + ka2) + lam3*(-(*ka)*lam1 + lam2*(-(*ka) + lam1) + ka2) - ka3) + eiL1*(*r1)*(-(*ka)*(*k23)*lam1 + (*ka)*(*k42)*(*k23))/(-(*ka)*lam13 + lam2*((*ka)*lam12 - lam13) + lam3*((*ka)*lam12 + lam2*(-(*ka)*lam1 + lam12) - lam13) + lam14) - eiL2*(*r1)*(-(*ka)*(*k23)*lam2 + (*ka)*(*k42)*(*k23))/(lam23*((*ka) + lam1) + lam3*(lam22*(-(*ka) - lam1) + (*ka)*lam2*lam1 + lam23) - (*ka)*lam22*lam1 - lam24) + eiL3*(*r1)*(-(*ka)*(*k23)*lam3 + (*ka)*(*k42)*(*k23))/(lam32*((*ka)*lam1 + lam2*((*ka) + lam1)) + (-(*ka) - lam1 - lam2)*lam33 - (*ka)*lam2*lam1*lam3 + lam34) + (*r1)*(*k42)*(*k23)/(lam2*lam1*lam3)) + (*ka)*(*k23)*(eL1*(E4 - lam1)/((-lam1 + lam3)*(-lam1 + lam2)*((*ka) - lam1)) + eL2*(E4 - lam2)/((-lam2 + lam3)*(lam1 - lam2)*((*ka) - lam2)) + eL3*(E4 - lam3)/((lam2 - lam3)*(lam1 - lam3)*((*ka) - lam3)) + eKa*(E4 - (*ka))/((-(*ka) + lam1)*(-(*ka) + lam2)*(-(*ka) + lam3)))*((*r1)/(*ka) - eiKa*(*r1)/(*ka)) + eL1*(E4*(*k23)*(-eiKa*(*r1)*((*k42)*(*k32) + (-(*k32) - (*k42))*(*ka) + ka2)/(ka2*lam1 + lam2*(-(*ka)*lam1 + ka2) + lam3*(-(*ka)*lam1 + lam2*(-(*ka) + lam1) + ka2) - ka3) - eiL1*(*r1)*(-(*ka)*lam12 - (*ka)*(*k42)*(*k32) + ((*k32) + (*k42))*(*ka)*lam1)/(-(*ka)*lam13 + lam2*((*ka)*lam12 - lam13) + lam3*((*ka)*lam12 + lam2*(-(*ka)*lam1 + lam12) - lam13) + lam14) + eiL2*(*r1)*(-(*ka)*lam22 - (*ka)*(*k42)*(*k32) + ((*k32) + (*k42))*(*ka)*lam2)/(lam23*((*ka) + lam1) + lam3*(lam22*(-(*ka) - lam1) + (*ka)*lam2*lam1 + lam23) - (*ka)*lam22*lam1 - lam24) - eiL3*(*r1)*(-(*ka)*lam32 - (*ka)*(*k42)*(*k32) + ((*k32) + (*k42))*(*ka)*lam3)/(lam32*((*ka)*lam1 + lam2*((*ka) + lam1)) + (-(*ka) - lam1 - lam2)*lam33 - (*ka)*lam2*lam1*lam3 + lam34) + (*r1)*(*k42)*(*k32)/(lam2*lam1*lam3)) - (*k23)*lam1*(-eiKa*(*r1)*((*k42)*(*k32) + (-(*k32) - (*k42))*(*ka) + ka2)/(ka2*lam1 + lam2*(-(*ka)*lam1 + ka2) + lam3*(-(*ka)*lam1 + lam2*(-(*ka) + lam1) + ka2) - ka3) - eiL1*(*r1)*(-(*ka)*lam12 - (*ka)*(*k42)*(*k32) + ((*k32) + (*k42))*(*ka)*lam1)/(-(*ka)*lam13 + lam2*((*ka)*lam12 - lam13) + lam3*((*ka)*lam12 + lam2*(-(*ka)*lam1 + lam12) - lam13) + lam14) + eiL2*(*r1)*(-(*ka)*lam22 - (*ka)*(*k42)*(*k32) + ((*k32) + (*k42))*(*ka)*lam2)/(lam23*((*ka) + lam1) + lam3*(lam22*(-(*ka) - lam1) + (*ka)*lam2*lam1 + lam23) - (*ka)*lam22*lam1 - lam24) - eiL3*(*r1)*(-(*ka)*lam32 - (*ka)*(*k42)*(*k32) + ((*k32) + (*k42))*(*ka)*lam3)/(lam32*((*ka)*lam1 + lam2*((*ka) + lam1)) + (-(*ka) - lam1 - lam2)*lam33 - (*ka)*lam2*lam1*lam3 + lam34) + (*r1)*(*k42)*(*k32)/(lam2*lam1*lam3)) + (*k42)*(*k23)*(eiKa*(*r1)*(-(*k24)*(*k32) + (*ka)*(*k24))/(ka2*lam1 + lam2*(-(*ka)*lam1 + ka2) + lam3*(-(*ka)*lam1 + lam2*(-(*ka) + lam1) + ka2) - ka3) + eiL1*(*r1)*((*ka)*(*k24)*(*k32) - (*ka)*(*k24)*lam1)/(-(*ka)*lam13 + lam2*((*ka)*lam12 - lam13) + lam3*((*ka)*lam12 + lam2*(-(*ka)*lam1 + lam12) - lam13) + lam14) - eiL2*(*r1)*((*ka)*(*k24)*(*k32) - (*ka)*(*k24)*lam2)/(lam23*((*ka) + lam1) + lam3*(lam22*(-(*ka) - lam1) + (*ka)*lam2*lam1 + lam23) - (*ka)*lam22*lam1 - lam24) + eiL3*(*r1)*((*ka)*(*k24)*(*k32) - (*ka)*(*k24)*lam3)/(lam32*((*ka)*lam1 + lam2*((*ka) + lam1)) + (-(*ka) - lam1 - lam2)*lam33 - (*ka)*lam2*lam1*lam3 + lam34) + (*r1)*(*k24)*(*k32)/(lam2*lam1*lam3)) - (*k42)*(*k24)*(eiKa*(*r1)*(-(*k42)*(*k23) + (*ka)*(*k23))/(ka2*lam1 + lam2*(-(*ka)*lam1 + ka2) + lam3*(-(*ka)*lam1 + lam2*(-(*ka) + lam1) + ka2) - ka3) + eiL1*(*r1)*(-(*ka)*(*k23)*lam1 + (*ka)*(*k42)*(*k23))/(-(*ka)*lam13 + lam2*((*ka)*lam12 - lam13) + lam3*((*ka)*lam12 + lam2*(-(*ka)*lam1 + lam12) - lam13) + lam14) - eiL2*(*r1)*(-(*ka)*(*k23)*lam2 + (*ka)*(*k42)*(*k23))/(lam23*((*ka) + lam1) + lam3*(lam22*(-(*ka) - lam1) + (*ka)*lam2*lam1 + lam23) - (*ka)*lam22*lam1 - lam24) + eiL3*(*r1)*(-(*ka)*(*k23)*lam3 + (*ka)*(*k42)*(*k23))/(lam32*((*ka)*lam1 + lam2*((*ka) + lam1)) + (-(*ka) - lam1 - lam2)*lam33 - (*ka)*lam2*lam1*lam3 + lam34) + (*r1)*(*k42)*(*k23)/(lam2*lam1*lam3)))/((lam1 - lam3)*(lam1 - lam2)) + eL3*((*k23)*lam3*(-eiKa*(*r1)*((*k42)*(*k32) + (-(*k32) - (*k42))*(*ka) + ka2)/(ka2*lam1 + lam2*(-(*ka)*lam1 + ka2) + lam3*(-(*ka)*lam1 + lam2*(-(*ka) + lam1) + ka2) - ka3) - eiL1*(*r1)*(-(*ka)*lam12 - (*ka)*(*k42)*(*k32) + ((*k32) + (*k42))*(*ka)*lam1)/(-(*ka)*lam13 + lam2*((*ka)*lam12 - lam13) + lam3*((*ka)*lam12 + lam2*(-(*ka)*lam1 + lam12) - lam13) + lam14) + eiL2*(*r1)*(-(*ka)*lam22 - (*ka)*(*k42)*(*k32) + ((*k32) + (*k42))*(*ka)*lam2)/(lam23*((*ka) + lam1) + lam3*(lam22*(-(*ka) - lam1) + (*ka)*lam2*lam1 + lam23) - (*ka)*lam22*lam1 - lam24) - eiL3*(*r1)*(-(*ka)*lam32 - (*ka)*(*k42)*(*k32) + ((*k32) + (*k42))*(*ka)*lam3)/(lam32*((*ka)*lam1 + lam2*((*ka) + lam1)) + (-(*ka) - lam1 - lam2)*lam33 - (*ka)*lam2*lam1*lam3 + lam34) + (*r1)*(*k42)*(*k32)/(lam2*lam1*lam3)) - (E4*(*k23)*(-eiKa*(*r1)*((*k42)*(*k32) + (-(*k32) - (*k42))*(*ka) + ka2)/(ka2*lam1 + lam2*(-(*ka)*lam1 + ka2) + lam3*(-(*ka)*lam1 + lam2*(-(*ka) + lam1) + ka2) - ka3) - eiL1*(*r1)*(-(*ka)*lam12 - (*ka)*(*k42)*(*k32) + ((*k32) + (*k42))*(*ka)*lam1)/(-(*ka)*lam13 + lam2*((*ka)*lam12 - lam13) + lam3*((*ka)*lam12 + lam2*(-(*ka)*lam1 + lam12) - lam13) + lam14) + eiL2*(*r1)*(-(*ka)*lam22 - (*ka)*(*k42)*(*k32) + ((*k32) + (*k42))*(*ka)*lam2)/(lam23*((*ka) + lam1) + lam3*(lam22*(-(*ka) - lam1) + (*ka)*lam2*lam1 + lam23) - (*ka)*lam22*lam1 - lam24) - eiL3*(*r1)*(-(*ka)*lam32 - (*ka)*(*k42)*(*k32) + ((*k32) + (*k42))*(*ka)*lam3)/(lam32*((*ka)*lam1 + lam2*((*ka) + lam1)) + (-(*ka) - lam1 - lam2)*lam33 - (*ka)*lam2*lam1*lam3 + lam34) + (*r1)*(*k42)*(*k32)/(lam2*lam1*lam3)) + (*k42)*(*k23)*(eiKa*(*r1)*(-(*k24)*(*k32) + (*ka)*(*k24))/(ka2*lam1 + lam2*(-(*ka)*lam1 + ka2) + lam3*(-(*ka)*lam1 + lam2*(-(*ka) + lam1) + ka2) - ka3) + eiL1*(*r1)*((*ka)*(*k24)*(*k32) - (*ka)*(*k24)*lam1)/(-(*ka)*lam13 + lam2*((*ka)*lam12 - lam13) + lam3*((*ka)*lam12 + lam2*(-(*ka)*lam1 + lam12) - lam13) + lam14) - eiL2*(*r1)*((*ka)*(*k24)*(*k32) - (*ka)*(*k24)*lam2)/(lam23*((*ka) + lam1) + lam3*(lam22*(-(*ka) - lam1) + (*ka)*lam2*lam1 + lam23) - (*ka)*lam22*lam1 - lam24) + eiL3*(*r1)*((*ka)*(*k24)*(*k32) - (*ka)*(*k24)*lam3)/(lam32*((*ka)*lam1 + lam2*((*ka) + lam1)) + (-(*ka) - lam1 - lam2)*lam33 - (*ka)*lam2*lam1*lam3 + lam34) + (*r1)*(*k24)*(*k32)/(lam2*lam1*lam3)) - (*k42)*(*k24)*(eiKa*(*r1)*(-(*k42)*(*k23) + (*ka)*(*k23))/(ka2*lam1 + lam2*(-(*ka)*lam1 + ka2) + lam3*(-(*ka)*lam1 + lam2*(-(*ka) + lam1) + ka2) - ka3) + eiL1*(*r1)*(-(*ka)*(*k23)*lam1 + (*ka)*(*k42)*(*k23))/(-(*ka)*lam13 + lam2*((*ka)*lam12 - lam13) + lam3*((*ka)*lam12 + lam2*(-(*ka)*lam1 + lam12) - lam13) + lam14) - eiL2*(*r1)*(-(*ka)*(*k23)*lam2 + (*ka)*(*k42)*(*k23))/(lam23*((*ka) + lam1) + lam3*(lam22*(-(*ka) - lam1) + (*ka)*lam2*lam1 + lam23) - (*ka)*lam22*lam1 - lam24) + eiL3*(*r1)*(-(*ka)*(*k23)*lam3 + (*ka)*(*k42)*(*k23))/(lam32*((*ka)*lam1 + lam2*((*ka) + lam1)) + (-(*ka) - lam1 - lam2)*lam33 - (*ka)*lam2*lam1*lam3 + lam34) + (*r1)*(*k42)*(*k23)/(lam2*lam1*lam3))))/((-lam2 + lam3)*(lam1 - lam3)) + eL2*((*k23)*lam2*(-eiKa*(*r1)*((*k42)*(*k32) + (-(*k32) - (*k42))*(*ka) + ka2)/(ka2*lam1 + lam2*(-(*ka)*lam1 + ka2) + lam3*(-(*ka)*lam1 + lam2*(-(*ka) + lam1) + ka2) - ka3) - eiL1*(*r1)*(-(*ka)*lam12 - (*ka)*(*k42)*(*k32) + ((*k32) + (*k42))*(*ka)*lam1)/(-(*ka)*lam13 + lam2*((*ka)*lam12 - lam13) + lam3*((*ka)*lam12 + lam2*(-(*ka)*lam1 + lam12) - lam13) + lam14) + eiL2*(*r1)*(-(*ka)*lam22 - (*ka)*(*k42)*(*k32) + ((*k32) + (*k42))*(*ka)*lam2)/(lam23*((*ka) + lam1) + lam3*(lam22*(-(*ka) - lam1) + (*ka)*lam2*lam1 + lam23) - (*ka)*lam22*lam1 - lam24) - eiL3*(*r1)*(-(*ka)*lam32 - (*ka)*(*k42)*(*k32) + ((*k32) + (*k42))*(*ka)*lam3)/(lam32*((*ka)*lam1 + lam2*((*ka) + lam1)) + (-(*ka) - lam1 - lam2)*lam33 - (*ka)*lam2*lam1*lam3 + lam34) + (*r1)*(*k42)*(*k32)/(lam2*lam1*lam3)) - (E4*(*k23)*(-eiKa*(*r1)*((*k42)*(*k32) + (-(*k32) - (*k42))*(*ka) + ka2)/(ka2*lam1 + lam2*(-(*ka)*lam1 + ka2) + lam3*(-(*ka)*lam1 + lam2*(-(*ka) + lam1) + ka2) - ka3) - eiL1*(*r1)*(-(*ka)*lam12 - (*ka)*(*k42)*(*k32) + ((*k32) + (*k42))*(*ka)*lam1)/(-(*ka)*lam13 + lam2*((*ka)*lam12 - lam13) + lam3*((*ka)*lam12 + lam2*(-(*ka)*lam1 + lam12) - lam13) + lam14) + eiL2*(*r1)*(-(*ka)*lam22 - (*ka)*(*k42)*(*k32) + ((*k32) + (*k42))*(*ka)*lam2)/(lam23*((*ka) + lam1) + lam3*(lam22*(-(*ka) - lam1) + (*ka)*lam2*lam1 + lam23) - (*ka)*lam22*lam1 - lam24) - eiL3*(*r1)*(-(*ka)*lam32 - (*ka)*(*k42)*(*k32) + ((*k32) + (*k42))*(*ka)*lam3)/(lam32*((*ka)*lam1 + lam2*((*ka) + lam1)) + (-(*ka) - lam1 - lam2)*lam33 - (*ka)*lam2*lam1*lam3 + lam34) + (*r1)*(*k42)*(*k32)/(lam2*lam1*lam3)) + (*k42)*(*k23)*(eiKa*(*r1)*(-(*k24)*(*k32) + (*ka)*(*k24))/(ka2*lam1 + lam2*(-(*ka)*lam1 + ka2) + lam3*(-(*ka)*lam1 + lam2*(-(*ka) + lam1) + ka2) - ka3) + eiL1*(*r1)*((*ka)*(*k24)*(*k32) - (*ka)*(*k24)*lam1)/(-(*ka)*lam13 + lam2*((*ka)*lam12 - lam13) + lam3*((*ka)*lam12 + lam2*(-(*ka)*lam1 + lam12) - lam13) + lam14) - eiL2*(*r1)*((*ka)*(*k24)*(*k32) - (*ka)*(*k24)*lam2)/(lam23*((*ka) + lam1) + lam3*(lam22*(-(*ka) - lam1) + (*ka)*lam2*lam1 + lam23) - (*ka)*lam22*lam1 - lam24) + eiL3*(*r1)*((*ka)*(*k24)*(*k32) - (*ka)*(*k24)*lam3)/(lam32*((*ka)*lam1 + lam2*((*ka) + lam1)) + (-(*ka) - lam1 - lam2)*lam33 - (*ka)*lam2*lam1*lam3 + lam34) + (*r1)*(*k24)*(*k32)/(lam2*lam1*lam3)) - (*k42)*(*k24)*(eiKa*(*r1)*(-(*k42)*(*k23) + (*ka)*(*k23))/(ka2*lam1 + lam2*(-(*ka)*lam1 + ka2) + lam3*(-(*ka)*lam1 + lam2*(-(*ka) + lam1) + ka2) - ka3) + eiL1*(*r1)*(-(*ka)*(*k23)*lam1 + (*ka)*(*k42)*(*k23))/(-(*ka)*lam13 + lam2*((*ka)*lam12 - lam13) + lam3*((*ka)*lam12 + lam2*(-(*ka)*lam1 + lam12) - lam13) + lam14) - eiL2*(*r1)*(-(*ka)*(*k23)*lam2 + (*ka)*(*k42)*(*k23))/(lam23*((*ka) + lam1) + lam3*(lam22*(-(*ka) - lam1) + (*ka)*lam2*lam1 + lam23) - (*ka)*lam22*lam1 - lam24) + eiL3*(*r1)*(-(*ka)*(*k23)*lam3 + (*ka)*(*k42)*(*k23))/(lam32*((*ka)*lam1 + lam2*((*ka) + lam1)) + (-(*ka) - lam1 - lam2)*lam33 - (*ka)*lam2*lam1*lam3 + lam34) + (*r1)*(*k42)*(*k23)/(lam2*lam1*lam3))))/((lam2 - lam3)*(lam1 - lam2));
  A4=(eL1*(E3 - lam1)*(E2 - lam1)/((-lam1 + lam3)*(-lam1 + lam2)) + eL2*(E2 - lam2)*(E3 - lam2)/((-lam2 + lam3)*(lam1 - lam2)) + eL3*(E2 - lam3)*(E3 - lam3)/((lam2 - lam3)*(lam1 - lam3)))*(eiKa*(*r1)*(-(*k24)*(*k32) + (*ka)*(*k24))/(ka2*lam1 + lam2*(-(*ka)*lam1 + ka2) + lam3*(-(*ka)*lam1 + lam2*(-(*ka) + lam1) + ka2) - ka3) + eiL1*(*r1)*((*ka)*(*k24)*(*k32) - (*ka)*(*k24)*lam1)/(-(*ka)*lam13 + lam2*((*ka)*lam12 - lam13) + lam3*((*ka)*lam12 + lam2*(-(*ka)*lam1 + lam12) - lam13) + lam14) - eiL2*(*r1)*((*ka)*(*k24)*(*k32) - (*ka)*(*k24)*lam2)/(lam23*((*ka) + lam1) + lam3*(lam22*(-(*ka) - lam1) + (*ka)*lam2*lam1 + lam23) - (*ka)*lam22*lam1 - lam24) + eiL3*(*r1)*((*ka)*(*k24)*(*k32) - (*ka)*(*k24)*lam3)/(lam32*((*ka)*lam1 + lam2*((*ka) + lam1)) + (-(*ka) - lam1 - lam2)*lam33 - (*ka)*lam2*lam1*lam3 + lam34) + (*r1)*(*k24)*(*k32)/(lam2*lam1*lam3)) + (*ka)*(*k24)*(eL1*(E3 - lam1)/((-lam1 + lam3)*(-lam1 + lam2)*((*ka) - lam1)) + eL2*(E3 - lam2)/((-lam2 + lam3)*(lam1 - lam2)*((*ka) - lam2)) + eL3*(E3 - lam3)/((lam2 - lam3)*(lam1 - lam3)*((*ka) - lam3)) + eKa*(E3 - (*ka))/((-(*ka) + lam1)*(-(*ka) + lam2)*(-(*ka) + lam3)))*((*r1)/(*ka) - eiKa*(*r1)/(*ka)) + eL1*(E3*(*k24)*(-eiKa*(*r1)*((*k42)*(*k32) + (-(*k32) - (*k42))*(*ka) + ka2)/(ka2*lam1 + lam2*(-(*ka)*lam1 + ka2) + lam3*(-(*ka)*lam1 + lam2*(-(*ka) + lam1) + ka2) - ka3) - eiL1*(*r1)*(-(*ka)*lam12 - (*ka)*(*k42)*(*k32) + ((*k32) + (*k42))*(*ka)*lam1)/(-(*ka)*lam13 + lam2*((*ka)*lam12 - lam13) + lam3*((*ka)*lam12 + lam2*(-(*ka)*lam1 + lam12) - lam13) + lam14) + eiL2*(*r1)*(-(*ka)*lam22 - (*ka)*(*k42)*(*k32) + ((*k32) + (*k42))*(*ka)*lam2)/(lam23*((*ka) + lam1) + lam3*(lam22*(-(*ka) - lam1) + (*ka)*lam2*lam1 + lam23) - (*ka)*lam22*lam1 - lam24) - eiL3*(*r1)*(-(*ka)*lam32 - (*ka)*(*k42)*(*k32) + ((*k32) + (*k42))*(*ka)*lam3)/(lam32*((*ka)*lam1 + lam2*((*ka) + lam1)) + (-(*ka) - lam1 - lam2)*lam33 - (*ka)*lam2*lam1*lam3 + lam34) + (*r1)*(*k42)*(*k32)/(lam2*lam1*lam3)) - (*k23)*(*k32)*(eiKa*(*r1)*(-(*k24)*(*k32) + (*ka)*(*k24))/(ka2*lam1 + lam2*(-(*ka)*lam1 + ka2) + lam3*(-(*ka)*lam1 + lam2*(-(*ka) + lam1) + ka2) - ka3) + eiL1*(*r1)*((*ka)*(*k24)*(*k32) - (*ka)*(*k24)*lam1)/(-(*ka)*lam13 + lam2*((*ka)*lam12 - lam13) + lam3*((*ka)*lam12 + lam2*(-(*ka)*lam1 + lam12) - lam13) + lam14) - eiL2*(*r1)*((*ka)*(*k24)*(*k32) - (*ka)*(*k24)*lam2)/(lam23*((*ka) + lam1) + lam3*(lam22*(-(*ka) - lam1) + (*ka)*lam2*lam1 + lam23) - (*ka)*lam22*lam1 - lam24) + eiL3*(*r1)*((*ka)*(*k24)*(*k32) - (*ka)*(*k24)*lam3)/(lam32*((*ka)*lam1 + lam2*((*ka) + lam1)) + (-(*ka) - lam1 - lam2)*lam33 - (*ka)*lam2*lam1*lam3 + lam34) + (*r1)*(*k24)*(*k32)/(lam2*lam1*lam3)) + (*k24)*(*k32)*(eiKa*(*r1)*(-(*k42)*(*k23) + (*ka)*(*k23))/(ka2*lam1 + lam2*(-(*ka)*lam1 + ka2) + lam3*(-(*ka)*lam1 + lam2*(-(*ka) + lam1) + ka2) - ka3) + eiL1*(*r1)*(-(*ka)*(*k23)*lam1 + (*ka)*(*k42)*(*k23))/(-(*ka)*lam13 + lam2*((*ka)*lam12 - lam13) + lam3*((*ka)*lam12 + lam2*(-(*ka)*lam1 + lam12) - lam13) + lam14) - eiL2*(*r1)*(-(*ka)*(*k23)*lam2 + (*ka)*(*k42)*(*k23))/(lam23*((*ka) + lam1) + lam3*(lam22*(-(*ka) - lam1) + (*ka)*lam2*lam1 + lam23) - (*ka)*lam22*lam1 - lam24) + eiL3*(*r1)*(-(*ka)*(*k23)*lam3 + (*ka)*(*k42)*(*k23))/(lam32*((*ka)*lam1 + lam2*((*ka) + lam1)) + (-(*ka) - lam1 - lam2)*lam33 - (*ka)*lam2*lam1*lam3 + lam34) + (*r1)*(*k42)*(*k23)/(lam2*lam1*lam3)) - (*k24)*lam1*(-eiKa*(*r1)*((*k42)*(*k32) + (-(*k32) - (*k42))*(*ka) + ka2)/(ka2*lam1 + lam2*(-(*ka)*lam1 + ka2) + lam3*(-(*ka)*lam1 + lam2*(-(*ka) + lam1) + ka2) - ka3) - eiL1*(*r1)*(-(*ka)*lam12 - (*ka)*(*k42)*(*k32) + ((*k32) + (*k42))*(*ka)*lam1)/(-(*ka)*lam13 + lam2*((*ka)*lam12 - lam13) + lam3*((*ka)*lam12 + lam2*(-(*ka)*lam1 + lam12) - lam13) + lam14) + eiL2*(*r1)*(-(*ka)*lam22 - (*ka)*(*k42)*(*k32) + ((*k32) + (*k42))*(*ka)*lam2)/(lam23*((*ka) + lam1) + lam3*(lam22*(-(*ka) - lam1) + (*ka)*lam2*lam1 + lam23) - (*ka)*lam22*lam1 - lam24) - eiL3*(*r1)*(-(*ka)*lam32 - (*ka)*(*k42)*(*k32) + ((*k32) + (*k42))*(*ka)*lam3)/(lam32*((*ka)*lam1 + lam2*((*ka) + lam1)) + (-(*ka) - lam1 - lam2)*lam33 - (*ka)*lam2*lam1*lam3 + lam34) + (*r1)*(*k42)*(*k32)/(lam2*lam1*lam3)))/((lam1 - lam3)*(lam1 - lam2)) + eL3*((*k24)*lam3*(-eiKa*(*r1)*((*k42)*(*k32) + (-(*k32) - (*k42))*(*ka) + ka2)/(ka2*lam1 + lam2*(-(*ka)*lam1 + ka2) + lam3*(-(*ka)*lam1 + lam2*(-(*ka) + lam1) + ka2) - ka3) - eiL1*(*r1)*(-(*ka)*lam12 - (*ka)*(*k42)*(*k32) + ((*k32) + (*k42))*(*ka)*lam1)/(-(*ka)*lam13 + lam2*((*ka)*lam12 - lam13) + lam3*((*ka)*lam12 + lam2*(-(*ka)*lam1 + lam12) - lam13) + lam14) + eiL2*(*r1)*(-(*ka)*lam22 - (*ka)*(*k42)*(*k32) + ((*k32) + (*k42))*(*ka)*lam2)/(lam23*((*ka) + lam1) + lam3*(lam22*(-(*ka) - lam1) + (*ka)*lam2*lam1 + lam23) - (*ka)*lam22*lam1 - lam24) - eiL3*(*r1)*(-(*ka)*lam32 - (*ka)*(*k42)*(*k32) + ((*k32) + (*k42))*(*ka)*lam3)/(lam32*((*ka)*lam1 + lam2*((*ka) + lam1)) + (-(*ka) - lam1 - lam2)*lam33 - (*ka)*lam2*lam1*lam3 + lam34) + (*r1)*(*k42)*(*k32)/(lam2*lam1*lam3)) - (E3*(*k24)*(-eiKa*(*r1)*((*k42)*(*k32) + (-(*k32) - (*k42))*(*ka) + ka2)/(ka2*lam1 + lam2*(-(*ka)*lam1 + ka2) + lam3*(-(*ka)*lam1 + lam2*(-(*ka) + lam1) + ka2) - ka3) - eiL1*(*r1)*(-(*ka)*lam12 - (*ka)*(*k42)*(*k32) + ((*k32) + (*k42))*(*ka)*lam1)/(-(*ka)*lam13 + lam2*((*ka)*lam12 - lam13) + lam3*((*ka)*lam12 + lam2*(-(*ka)*lam1 + lam12) - lam13) + lam14) + eiL2*(*r1)*(-(*ka)*lam22 - (*ka)*(*k42)*(*k32) + ((*k32) + (*k42))*(*ka)*lam2)/(lam23*((*ka) + lam1) + lam3*(lam22*(-(*ka) - lam1) + (*ka)*lam2*lam1 + lam23) - (*ka)*lam22*lam1 - lam24) - eiL3*(*r1)*(-(*ka)*lam32 - (*ka)*(*k42)*(*k32) + ((*k32) + (*k42))*(*ka)*lam3)/(lam32*((*ka)*lam1 + lam2*((*ka) + lam1)) + (-(*ka) - lam1 - lam2)*lam33 - (*ka)*lam2*lam1*lam3 + lam34) + (*r1)*(*k42)*(*k32)/(lam2*lam1*lam3)) - (*k23)*(*k32)*(eiKa*(*r1)*(-(*k24)*(*k32) + (*ka)*(*k24))/(ka2*lam1 + lam2*(-(*ka)*lam1 + ka2) + lam3*(-(*ka)*lam1 + lam2*(-(*ka) + lam1) + ka2) - ka3) + eiL1*(*r1)*((*ka)*(*k24)*(*k32) - (*ka)*(*k24)*lam1)/(-(*ka)*lam13 + lam2*((*ka)*lam12 - lam13) + lam3*((*ka)*lam12 + lam2*(-(*ka)*lam1 + lam12) - lam13) + lam14) - eiL2*(*r1)*((*ka)*(*k24)*(*k32) - (*ka)*(*k24)*lam2)/(lam23*((*ka) + lam1) + lam3*(lam22*(-(*ka) - lam1) + (*ka)*lam2*lam1 + lam23) - (*ka)*lam22*lam1 - lam24) + eiL3*(*r1)*((*ka)*(*k24)*(*k32) - (*ka)*(*k24)*lam3)/(lam32*((*ka)*lam1 + lam2*((*ka) + lam1)) + (-(*ka) - lam1 - lam2)*lam33 - (*ka)*lam2*lam1*lam3 + lam34) + (*r1)*(*k24)*(*k32)/(lam2*lam1*lam3)) + (*k24)*(*k32)*(eiKa*(*r1)*(-(*k42)*(*k23) + (*ka)*(*k23))/(ka2*lam1 + lam2*(-(*ka)*lam1 + ka2) + lam3*(-(*ka)*lam1 + lam2*(-(*ka) + lam1) + ka2) - ka3) + eiL1*(*r1)*(-(*ka)*(*k23)*lam1 + (*ka)*(*k42)*(*k23))/(-(*ka)*lam13 + lam2*((*ka)*lam12 - lam13) + lam3*((*ka)*lam12 + lam2*(-(*ka)*lam1 + lam12) - lam13) + lam14) - eiL2*(*r1)*(-(*ka)*(*k23)*lam2 + (*ka)*(*k42)*(*k23))/(lam23*((*ka) + lam1) + lam3*(lam22*(-(*ka) - lam1) + (*ka)*lam2*lam1 + lam23) - (*ka)*lam22*lam1 - lam24) + eiL3*(*r1)*(-(*ka)*(*k23)*lam3 + (*ka)*(*k42)*(*k23))/(lam32*((*ka)*lam1 + lam2*((*ka) + lam1)) + (-(*ka) - lam1 - lam2)*lam33 - (*ka)*lam2*lam1*lam3 + lam34) + (*r1)*(*k42)*(*k23)/(lam2*lam1*lam3))))/((-lam2 + lam3)*(lam1 - lam3)) + eL2*((*k24)*lam2*(-eiKa*(*r1)*((*k42)*(*k32) + (-(*k32) - (*k42))*(*ka) + ka2)/(ka2*lam1 + lam2*(-(*ka)*lam1 + ka2) + lam3*(-(*ka)*lam1 + lam2*(-(*ka) + lam1) + ka2) - ka3) - eiL1*(*r1)*(-(*ka)*lam12 - (*ka)*(*k42)*(*k32) + ((*k32) + (*k42))*(*ka)*lam1)/(-(*ka)*lam13 + lam2*((*ka)*lam12 - lam13) + lam3*((*ka)*lam12 + lam2*(-(*ka)*lam1 + lam12) - lam13) + lam14) + eiL2*(*r1)*(-(*ka)*lam22 - (*ka)*(*k42)*(*k32) + ((*k32) + (*k42))*(*ka)*lam2)/(lam23*((*ka) + lam1) + lam3*(lam22*(-(*ka) - lam1) + (*ka)*lam2*lam1 + lam23) - (*ka)*lam22*lam1 - lam24) - eiL3*(*r1)*(-(*ka)*lam32 - (*ka)*(*k42)*(*k32) + ((*k32) + (*k42))*(*ka)*lam3)/(lam32*((*ka)*lam1 + lam2*((*ka) + lam1)) + (-(*ka) - lam1 - lam2)*lam33 - (*ka)*lam2*lam1*lam3 + lam34) + (*r1)*(*k42)*(*k32)/(lam2*lam1*lam3)) - (E3*(*k24)*(-eiKa*(*r1)*((*k42)*(*k32) + (-(*k32) - (*k42))*(*ka) + ka2)/(ka2*lam1 + lam2*(-(*ka)*lam1 + ka2) + lam3*(-(*ka)*lam1 + lam2*(-(*ka) + lam1) + ka2) - ka3) - eiL1*(*r1)*(-(*ka)*lam12 - (*ka)*(*k42)*(*k32) + ((*k32) + (*k42))*(*ka)*lam1)/(-(*ka)*lam13 + lam2*((*ka)*lam12 - lam13) + lam3*((*ka)*lam12 + lam2*(-(*ka)*lam1 + lam12) - lam13) + lam14) + eiL2*(*r1)*(-(*ka)*lam22 - (*ka)*(*k42)*(*k32) + ((*k32) + (*k42))*(*ka)*lam2)/(lam23*((*ka) + lam1) + lam3*(lam22*(-(*ka) - lam1) + (*ka)*lam2*lam1 + lam23) - (*ka)*lam22*lam1 - lam24) - eiL3*(*r1)*(-(*ka)*lam32 - (*ka)*(*k42)*(*k32) + ((*k32) + (*k42))*(*ka)*lam3)/(lam32*((*ka)*lam1 + lam2*((*ka) + lam1)) + (-(*ka) - lam1 - lam2)*lam33 - (*ka)*lam2*lam1*lam3 + lam34) + (*r1)*(*k42)*(*k32)/(lam2*lam1*lam3)) - (*k23)*(*k32)*(eiKa*(*r1)*(-(*k24)*(*k32) + (*ka)*(*k24))/(ka2*lam1 + lam2*(-(*ka)*lam1 + ka2) + lam3*(-(*ka)*lam1 + lam2*(-(*ka) + lam1) + ka2) - ka3) + eiL1*(*r1)*((*ka)*(*k24)*(*k32) - (*ka)*(*k24)*lam1)/(-(*ka)*lam13 + lam2*((*ka)*lam12 - lam13) + lam3*((*ka)*lam12 + lam2*(-(*ka)*lam1 + lam12) - lam13) + lam14) - eiL2*(*r1)*((*ka)*(*k24)*(*k32) - (*ka)*(*k24)*lam2)/(lam23*((*ka) + lam1) + lam3*(lam22*(-(*ka) - lam1) + (*ka)*lam2*lam1 + lam23) - (*ka)*lam22*lam1 - lam24) + eiL3*(*r1)*((*ka)*(*k24)*(*k32) - (*ka)*(*k24)*lam3)/(lam32*((*ka)*lam1 + lam2*((*ka) + lam1)) + (-(*ka) - lam1 - lam2)*lam33 - (*ka)*lam2*lam1*lam3 + lam34) + (*r1)*(*k24)*(*k32)/(lam2*lam1*lam3)) + (*k24)*(*k32)*(eiKa*(*r1)*(-(*k42)*(*k23) + (*ka)*(*k23))/(ka2*lam1 + lam2*(-(*ka)*lam1 + ka2) + lam3*(-(*ka)*lam1 + lam2*(-(*ka) + lam1) + ka2) - ka3) + eiL1*(*r1)*(-(*ka)*(*k23)*lam1 + (*ka)*(*k42)*(*k23))/(-(*ka)*lam13 + lam2*((*ka)*lam12 - lam13) + lam3*((*ka)*lam12 + lam2*(-(*ka)*lam1 + lam12) - lam13) + lam14) - eiL2*(*r1)*(-(*ka)*(*k23)*lam2 + (*ka)*(*k42)*(*k23))/(lam23*((*ka) + lam1) + lam3*(lam22*(-(*ka) - lam1) + (*ka)*lam2*lam1 + lam23) - (*ka)*lam22*lam1 - lam24) + eiL3*(*r1)*(-(*ka)*(*k23)*lam3 + (*ka)*(*k42)*(*k23))/(lam32*((*ka)*lam1 + lam2*((*ka) + lam1)) + (-(*ka) - lam1 - lam2)*lam33 - (*ka)*lam2*lam1*lam3 + lam34) + (*r1)*(*k42)*(*k23)/(lam2*lam1*lam3))))/((lam2 - lam3)*(lam1 - lam2));
}

static inline void threeCmtKaRateSStr2(double *A, double *tinf, double *tau, double *r2, 
				       double *ka, double *k20,
				       double *k23, double *k32,
				       double *k24, double *k42) {
  double E2 =  (*k20)+ (*k23) + (*k24);
  double E3 = (*k32);
  double E4 = (*k42);
  double j = (*k23)+(*k20)+(*k32)+(*k42)+(*k24);
  double k = (*k23)*(*k42)+(*k20)*(*k32)+(*k20)*(*k42)+(*k32)*(*k42)+(*k24)*(*k32);
  double l = (*k20)*(*k32)*(*k42);

  double m = 0.3333333333333333*(3.0*k - j*j);
  double n = 0.03703703703703703*(2.0*j*j*j - 9.0*j*k + 27.0*l);
  double Q = 0.25*n*n + 0.03703703703703703*m*m*m;

  double alpha = sqrt(-Q);
  double beta = -0.5*n;
  double rho=sqrt(beta*beta+alpha*alpha);
  double theta = atan2(alpha,beta);
  double ct3 = cos(0.3333333333333333*theta);
  double rho3 = R_pow(rho,0.3333333333333333);
  double st3 = 1.732050807568877193177*sin(0.3333333333333333*theta);
  double j3 = 0.3333333333333333*j;
  double lam1 = j3  + rho3*(ct3 + st3);
  double lam2 = j3 + rho3*(ct3 - st3);
  double lam3 = j3 -(2.0*rho3*ct3);

  double eL1 = exp(-lam1*((*tau)-(*tinf)))/(1.0-exp(-lam1*(*tau)));
  double eiL1 = exp(-lam1*(*tinf));

  double eL2 = exp(-lam2*((*tau)-(*tinf)))/(1.0-exp(-lam2*(*tau)));
  double eiL2 = exp(-lam2*(*tinf));

  double eL3 = exp(-lam3*((*tau)-(*tinf)))/(1.0-exp(-lam3*(*tau)));
  double eiL3 = exp(-lam3*(*tinf));

  double lam12 = lam1*lam1;
  double lam13 = lam12*lam1;
  double lam14 = lam13*lam1;

  double lam22 = lam2*lam2;
  double lam23 = lam22*lam2;
  double lam24 = lam23*lam2;

  double lam32 = lam3*lam3;
  double lam33 = lam32*lam3;
  double lam34 = lam33*lam3;
  A1=0.0;
  A2=(eL1*(E4 - lam1)*(E3 - lam1)/((-lam1 + lam3)*(-lam1 + lam2)) + eL2*(E4 - lam2)*(E3 - lam2)/((-lam2 + lam3)*(lam1 - lam2)) + eL3*(E3 - lam3)*(E4 - lam3)/((lam2 - lam3)*(lam1 - lam3)))*(-eiL1*(*r2)*(lam1*((*k42)*(*k32) + ((*k32) + (*k42))*(*ka)) + (-(*k32) - (*k42) - (*ka))*lam12 - (*ka)*(*k42)*(*k32) + lam13)/(-(*ka)*lam13 + lam2*((*ka)*lam12 - lam13) + lam3*((*ka)*lam12 + lam2*(-(*ka)*lam1 + lam12) - lam13) + lam14) + eiL2*(*r2)*(lam2*((*k42)*(*k32) + ((*k32) + (*k42))*(*ka)) + (-(*k32) - (*k42) - (*ka))*lam22 - (*ka)*(*k42)*(*k32) + lam23)/(lam23*((*ka) + lam1) + lam3*(lam22*(-(*ka) - lam1) + (*ka)*lam2*lam1 + lam23) - (*ka)*lam22*lam1 - lam24) - eiL3*(*r2)*(lam3*((*k42)*(*k32) + ((*k32) + (*k42))*(*ka)) + (-(*k32) - (*k42) - (*ka))*lam32 - (*ka)*(*k42)*(*k32) + lam33)/(lam32*((*ka)*lam1 + lam2*((*ka) + lam1)) + (-(*ka) - lam1 - lam2)*lam33 - (*ka)*lam2*lam1*lam3 + lam34) + (*r2)*(*k42)*(*k32)/(lam2*lam1*lam3)) + eL1*(-lam1*((*k32)*(eiL1*(*r2)*((*k23)*lam12 + lam1*(-(*k42)*(*k23) - (*ka)*(*k23)) + (*ka)*(*k42)*(*k23))/(-(*ka)*lam13 + lam2*((*ka)*lam12 - lam13) + lam3*((*ka)*lam12 + lam2*(-(*ka)*lam1 + lam12) - lam13) + lam14) - eiL2*(*r2)*((*k23)*lam22 + lam2*(-(*k42)*(*k23) - (*ka)*(*k23)) + (*ka)*(*k42)*(*k23))/(lam23*((*ka) + lam1) + lam3*(lam22*(-(*ka) - lam1) + (*ka)*lam2*lam1 + lam23) - (*ka)*lam22*lam1 - lam24) + eiL3*(*r2)*((*k23)*lam32 + lam3*(-(*k42)*(*k23) - (*ka)*(*k23)) + (*ka)*(*k42)*(*k23))/(lam32*((*ka)*lam1 + lam2*((*ka) + lam1)) + (-(*ka) - lam1 - lam2)*lam33 - (*ka)*lam2*lam1*lam3 + lam34) + (*r2)*(*k42)*(*k23)/(lam2*lam1*lam3)) + (*k42)*(eiL1*(*r2)*((*k24)*lam12 + lam1*(-(*k24)*(*k32) - (*ka)*(*k24)) + (*ka)*(*k24)*(*k32))/(-(*ka)*lam13 + lam2*((*ka)*lam12 - lam13) + lam3*((*ka)*lam12 + lam2*(-(*ka)*lam1 + lam12) - lam13) + lam14) - eiL2*(*r2)*((*k24)*lam22 + lam2*(-(*k24)*(*k32) - (*ka)*(*k24)) + (*ka)*(*k24)*(*k32))/(lam23*((*ka) + lam1) + lam3*(lam22*(-(*ka) - lam1) + (*ka)*lam2*lam1 + lam23) - (*ka)*lam22*lam1 - lam24) + eiL3*(*r2)*((*k24)*lam32 + lam3*(-(*k24)*(*k32) - (*ka)*(*k24)) + (*ka)*(*k24)*(*k32))/(lam32*((*ka)*lam1 + lam2*((*ka) + lam1)) + (-(*ka) - lam1 - lam2)*lam33 - (*ka)*lam2*lam1*lam3 + lam34) + (*r2)*(*k24)*(*k32)/(lam2*lam1*lam3))) + E3*(*k42)*(eiL1*(*r2)*((*k24)*lam12 + lam1*(-(*k24)*(*k32) - (*ka)*(*k24)) + (*ka)*(*k24)*(*k32))/(-(*ka)*lam13 + lam2*((*ka)*lam12 - lam13) + lam3*((*ka)*lam12 + lam2*(-(*ka)*lam1 + lam12) - lam13) + lam14) - eiL2*(*r2)*((*k24)*lam22 + lam2*(-(*k24)*(*k32) - (*ka)*(*k24)) + (*ka)*(*k24)*(*k32))/(lam23*((*ka) + lam1) + lam3*(lam22*(-(*ka) - lam1) + (*ka)*lam2*lam1 + lam23) - (*ka)*lam22*lam1 - lam24) + eiL3*(*r2)*((*k24)*lam32 + lam3*(-(*k24)*(*k32) - (*ka)*(*k24)) + (*ka)*(*k24)*(*k32))/(lam32*((*ka)*lam1 + lam2*((*ka) + lam1)) + (-(*ka) - lam1 - lam2)*lam33 - (*ka)*lam2*lam1*lam3 + lam34) + (*r2)*(*k24)*(*k32)/(lam2*lam1*lam3)) + E4*(*k32)*(eiL1*(*r2)*((*k23)*lam12 + lam1*(-(*k42)*(*k23) - (*ka)*(*k23)) + (*ka)*(*k42)*(*k23))/(-(*ka)*lam13 + lam2*((*ka)*lam12 - lam13) + lam3*((*ka)*lam12 + lam2*(-(*ka)*lam1 + lam12) - lam13) + lam14) - eiL2*(*r2)*((*k23)*lam22 + lam2*(-(*k42)*(*k23) - (*ka)*(*k23)) + (*ka)*(*k42)*(*k23))/(lam23*((*ka) + lam1) + lam3*(lam22*(-(*ka) - lam1) + (*ka)*lam2*lam1 + lam23) - (*ka)*lam22*lam1 - lam24) + eiL3*(*r2)*((*k23)*lam32 + lam3*(-(*k42)*(*k23) - (*ka)*(*k23)) + (*ka)*(*k42)*(*k23))/(lam32*((*ka)*lam1 + lam2*((*ka) + lam1)) + (-(*ka) - lam1 - lam2)*lam33 - (*ka)*lam2*lam1*lam3 + lam34) + (*r2)*(*k42)*(*k23)/(lam2*lam1*lam3)))/((lam1 - lam3)*(lam1 - lam2)) + eL3*(lam3*((*k32)*(eiL1*(*r2)*((*k23)*lam12 + lam1*(-(*k42)*(*k23) - (*ka)*(*k23)) + (*ka)*(*k42)*(*k23))/(-(*ka)*lam13 + lam2*((*ka)*lam12 - lam13) + lam3*((*ka)*lam12 + lam2*(-(*ka)*lam1 + lam12) - lam13) + lam14) - eiL2*(*r2)*((*k23)*lam22 + lam2*(-(*k42)*(*k23) - (*ka)*(*k23)) + (*ka)*(*k42)*(*k23))/(lam23*((*ka) + lam1) + lam3*(lam22*(-(*ka) - lam1) + (*ka)*lam2*lam1 + lam23) - (*ka)*lam22*lam1 - lam24) + eiL3*(*r2)*((*k23)*lam32 + lam3*(-(*k42)*(*k23) - (*ka)*(*k23)) + (*ka)*(*k42)*(*k23))/(lam32*((*ka)*lam1 + lam2*((*ka) + lam1)) + (-(*ka) - lam1 - lam2)*lam33 - (*ka)*lam2*lam1*lam3 + lam34) + (*r2)*(*k42)*(*k23)/(lam2*lam1*lam3)) + (*k42)*(eiL1*(*r2)*((*k24)*lam12 + lam1*(-(*k24)*(*k32) - (*ka)*(*k24)) + (*ka)*(*k24)*(*k32))/(-(*ka)*lam13 + lam2*((*ka)*lam12 - lam13) + lam3*((*ka)*lam12 + lam2*(-(*ka)*lam1 + lam12) - lam13) + lam14) - eiL2*(*r2)*((*k24)*lam22 + lam2*(-(*k24)*(*k32) - (*ka)*(*k24)) + (*ka)*(*k24)*(*k32))/(lam23*((*ka) + lam1) + lam3*(lam22*(-(*ka) - lam1) + (*ka)*lam2*lam1 + lam23) - (*ka)*lam22*lam1 - lam24) + eiL3*(*r2)*((*k24)*lam32 + lam3*(-(*k24)*(*k32) - (*ka)*(*k24)) + (*ka)*(*k24)*(*k32))/(lam32*((*ka)*lam1 + lam2*((*ka) + lam1)) + (-(*ka) - lam1 - lam2)*lam33 - (*ka)*lam2*lam1*lam3 + lam34) + (*r2)*(*k24)*(*k32)/(lam2*lam1*lam3))) - (E3*(*k42)*(eiL1*(*r2)*((*k24)*lam12 + lam1*(-(*k24)*(*k32) - (*ka)*(*k24)) + (*ka)*(*k24)*(*k32))/(-(*ka)*lam13 + lam2*((*ka)*lam12 - lam13) + lam3*((*ka)*lam12 + lam2*(-(*ka)*lam1 + lam12) - lam13) + lam14) - eiL2*(*r2)*((*k24)*lam22 + lam2*(-(*k24)*(*k32) - (*ka)*(*k24)) + (*ka)*(*k24)*(*k32))/(lam23*((*ka) + lam1) + lam3*(lam22*(-(*ka) - lam1) + (*ka)*lam2*lam1 + lam23) - (*ka)*lam22*lam1 - lam24) + eiL3*(*r2)*((*k24)*lam32 + lam3*(-(*k24)*(*k32) - (*ka)*(*k24)) + (*ka)*(*k24)*(*k32))/(lam32*((*ka)*lam1 + lam2*((*ka) + lam1)) + (-(*ka) - lam1 - lam2)*lam33 - (*ka)*lam2*lam1*lam3 + lam34) + (*r2)*(*k24)*(*k32)/(lam2*lam1*lam3)) + E4*(*k32)*(eiL1*(*r2)*((*k23)*lam12 + lam1*(-(*k42)*(*k23) - (*ka)*(*k23)) + (*ka)*(*k42)*(*k23))/(-(*ka)*lam13 + lam2*((*ka)*lam12 - lam13) + lam3*((*ka)*lam12 + lam2*(-(*ka)*lam1 + lam12) - lam13) + lam14) - eiL2*(*r2)*((*k23)*lam22 + lam2*(-(*k42)*(*k23) - (*ka)*(*k23)) + (*ka)*(*k42)*(*k23))/(lam23*((*ka) + lam1) + lam3*(lam22*(-(*ka) - lam1) + (*ka)*lam2*lam1 + lam23) - (*ka)*lam22*lam1 - lam24) + eiL3*(*r2)*((*k23)*lam32 + lam3*(-(*k42)*(*k23) - (*ka)*(*k23)) + (*ka)*(*k42)*(*k23))/(lam32*((*ka)*lam1 + lam2*((*ka) + lam1)) + (-(*ka) - lam1 - lam2)*lam33 - (*ka)*lam2*lam1*lam3 + lam34) + (*r2)*(*k42)*(*k23)/(lam2*lam1*lam3))))/((-lam2 + lam3)*(lam1 - lam3)) + eL2*(lam2*((*k32)*(eiL1*(*r2)*((*k23)*lam12 + lam1*(-(*k42)*(*k23) - (*ka)*(*k23)) + (*ka)*(*k42)*(*k23))/(-(*ka)*lam13 + lam2*((*ka)*lam12 - lam13) + lam3*((*ka)*lam12 + lam2*(-(*ka)*lam1 + lam12) - lam13) + lam14) - eiL2*(*r2)*((*k23)*lam22 + lam2*(-(*k42)*(*k23) - (*ka)*(*k23)) + (*ka)*(*k42)*(*k23))/(lam23*((*ka) + lam1) + lam3*(lam22*(-(*ka) - lam1) + (*ka)*lam2*lam1 + lam23) - (*ka)*lam22*lam1 - lam24) + eiL3*(*r2)*((*k23)*lam32 + lam3*(-(*k42)*(*k23) - (*ka)*(*k23)) + (*ka)*(*k42)*(*k23))/(lam32*((*ka)*lam1 + lam2*((*ka) + lam1)) + (-(*ka) - lam1 - lam2)*lam33 - (*ka)*lam2*lam1*lam3 + lam34) + (*r2)*(*k42)*(*k23)/(lam2*lam1*lam3)) + (*k42)*(eiL1*(*r2)*((*k24)*lam12 + lam1*(-(*k24)*(*k32) - (*ka)*(*k24)) + (*ka)*(*k24)*(*k32))/(-(*ka)*lam13 + lam2*((*ka)*lam12 - lam13) + lam3*((*ka)*lam12 + lam2*(-(*ka)*lam1 + lam12) - lam13) + lam14) - eiL2*(*r2)*((*k24)*lam22 + lam2*(-(*k24)*(*k32) - (*ka)*(*k24)) + (*ka)*(*k24)*(*k32))/(lam23*((*ka) + lam1) + lam3*(lam22*(-(*ka) - lam1) + (*ka)*lam2*lam1 + lam23) - (*ka)*lam22*lam1 - lam24) + eiL3*(*r2)*((*k24)*lam32 + lam3*(-(*k24)*(*k32) - (*ka)*(*k24)) + (*ka)*(*k24)*(*k32))/(lam32*((*ka)*lam1 + lam2*((*ka) + lam1)) + (-(*ka) - lam1 - lam2)*lam33 - (*ka)*lam2*lam1*lam3 + lam34) + (*r2)*(*k24)*(*k32)/(lam2*lam1*lam3))) - (E3*(*k42)*(eiL1*(*r2)*((*k24)*lam12 + lam1*(-(*k24)*(*k32) - (*ka)*(*k24)) + (*ka)*(*k24)*(*k32))/(-(*ka)*lam13 + lam2*((*ka)*lam12 - lam13) + lam3*((*ka)*lam12 + lam2*(-(*ka)*lam1 + lam12) - lam13) + lam14) - eiL2*(*r2)*((*k24)*lam22 + lam2*(-(*k24)*(*k32) - (*ka)*(*k24)) + (*ka)*(*k24)*(*k32))/(lam23*((*ka) + lam1) + lam3*(lam22*(-(*ka) - lam1) + (*ka)*lam2*lam1 + lam23) - (*ka)*lam22*lam1 - lam24) + eiL3*(*r2)*((*k24)*lam32 + lam3*(-(*k24)*(*k32) - (*ka)*(*k24)) + (*ka)*(*k24)*(*k32))/(lam32*((*ka)*lam1 + lam2*((*ka) + lam1)) + (-(*ka) - lam1 - lam2)*lam33 - (*ka)*lam2*lam1*lam3 + lam34) + (*r2)*(*k24)*(*k32)/(lam2*lam1*lam3)) + E4*(*k32)*(eiL1*(*r2)*((*k23)*lam12 + lam1*(-(*k42)*(*k23) - (*ka)*(*k23)) + (*ka)*(*k42)*(*k23))/(-(*ka)*lam13 + lam2*((*ka)*lam12 - lam13) + lam3*((*ka)*lam12 + lam2*(-(*ka)*lam1 + lam12) - lam13) + lam14) - eiL2*(*r2)*((*k23)*lam22 + lam2*(-(*k42)*(*k23) - (*ka)*(*k23)) + (*ka)*(*k42)*(*k23))/(lam23*((*ka) + lam1) + lam3*(lam22*(-(*ka) - lam1) + (*ka)*lam2*lam1 + lam23) - (*ka)*lam22*lam1 - lam24) + eiL3*(*r2)*((*k23)*lam32 + lam3*(-(*k42)*(*k23) - (*ka)*(*k23)) + (*ka)*(*k42)*(*k23))/(lam32*((*ka)*lam1 + lam2*((*ka) + lam1)) + (-(*ka) - lam1 - lam2)*lam33 - (*ka)*lam2*lam1*lam3 + lam34) + (*r2)*(*k42)*(*k23)/(lam2*lam1*lam3))))/((lam2 - lam3)*(lam1 - lam2));
  A3=(eL1*(E4 - lam1)*(E2 - lam1)/((-lam1 + lam3)*(-lam1 + lam2)) + eL2*(E4 - lam2)*(E2 - lam2)/((-lam2 + lam3)*(lam1 - lam2)) + eL3*(E2 - lam3)*(E4 - lam3)/((lam2 - lam3)*(lam1 - lam3)))*(eiL1*(*r2)*((*k23)*lam12 + lam1*(-(*k42)*(*k23) - (*ka)*(*k23)) + (*ka)*(*k42)*(*k23))/(-(*ka)*lam13 + lam2*((*ka)*lam12 - lam13) + lam3*((*ka)*lam12 + lam2*(-(*ka)*lam1 + lam12) - lam13) + lam14) - eiL2*(*r2)*((*k23)*lam22 + lam2*(-(*k42)*(*k23) - (*ka)*(*k23)) + (*ka)*(*k42)*(*k23))/(lam23*((*ka) + lam1) + lam3*(lam22*(-(*ka) - lam1) + (*ka)*lam2*lam1 + lam23) - (*ka)*lam22*lam1 - lam24) + eiL3*(*r2)*((*k23)*lam32 + lam3*(-(*k42)*(*k23) - (*ka)*(*k23)) + (*ka)*(*k42)*(*k23))/(lam32*((*ka)*lam1 + lam2*((*ka) + lam1)) + (-(*ka) - lam1 - lam2)*lam33 - (*ka)*lam2*lam1*lam3 + lam34) + (*r2)*(*k42)*(*k23)/(lam2*lam1*lam3)) + eL1*(E4*(*k23)*(-eiL1*(*r2)*(lam1*((*k42)*(*k32) + ((*k32) + (*k42))*(*ka)) + (-(*k32) - (*k42) - (*ka))*lam12 - (*ka)*(*k42)*(*k32) + lam13)/(-(*ka)*lam13 + lam2*((*ka)*lam12 - lam13) + lam3*((*ka)*lam12 + lam2*(-(*ka)*lam1 + lam12) - lam13) + lam14) + eiL2*(*r2)*(lam2*((*k42)*(*k32) + ((*k32) + (*k42))*(*ka)) + (-(*k32) - (*k42) - (*ka))*lam22 - (*ka)*(*k42)*(*k32) + lam23)/(lam23*((*ka) + lam1) + lam3*(lam22*(-(*ka) - lam1) + (*ka)*lam2*lam1 + lam23) - (*ka)*lam22*lam1 - lam24) - eiL3*(*r2)*(lam3*((*k42)*(*k32) + ((*k32) + (*k42))*(*ka)) + (-(*k32) - (*k42) - (*ka))*lam32 - (*ka)*(*k42)*(*k32) + lam33)/(lam32*((*ka)*lam1 + lam2*((*ka) + lam1)) + (-(*ka) - lam1 - lam2)*lam33 - (*ka)*lam2*lam1*lam3 + lam34) + (*r2)*(*k42)*(*k32)/(lam2*lam1*lam3)) - (*k23)*lam1*(-eiL1*(*r2)*(lam1*((*k42)*(*k32) + ((*k32) + (*k42))*(*ka)) + (-(*k32) - (*k42) - (*ka))*lam12 - (*ka)*(*k42)*(*k32) + lam13)/(-(*ka)*lam13 + lam2*((*ka)*lam12 - lam13) + lam3*((*ka)*lam12 + lam2*(-(*ka)*lam1 + lam12) - lam13) + lam14) + eiL2*(*r2)*(lam2*((*k42)*(*k32) + ((*k32) + (*k42))*(*ka)) + (-(*k32) - (*k42) - (*ka))*lam22 - (*ka)*(*k42)*(*k32) + lam23)/(lam23*((*ka) + lam1) + lam3*(lam22*(-(*ka) - lam1) + (*ka)*lam2*lam1 + lam23) - (*ka)*lam22*lam1 - lam24) - eiL3*(*r2)*(lam3*((*k42)*(*k32) + ((*k32) + (*k42))*(*ka)) + (-(*k32) - (*k42) - (*ka))*lam32 - (*ka)*(*k42)*(*k32) + lam33)/(lam32*((*ka)*lam1 + lam2*((*ka) + lam1)) + (-(*ka) - lam1 - lam2)*lam33 - (*ka)*lam2*lam1*lam3 + lam34) + (*r2)*(*k42)*(*k32)/(lam2*lam1*lam3)) + (*k42)*(*k23)*(eiL1*(*r2)*((*k24)*lam12 + lam1*(-(*k24)*(*k32) - (*ka)*(*k24)) + (*ka)*(*k24)*(*k32))/(-(*ka)*lam13 + lam2*((*ka)*lam12 - lam13) + lam3*((*ka)*lam12 + lam2*(-(*ka)*lam1 + lam12) - lam13) + lam14) - eiL2*(*r2)*((*k24)*lam22 + lam2*(-(*k24)*(*k32) - (*ka)*(*k24)) + (*ka)*(*k24)*(*k32))/(lam23*((*ka) + lam1) + lam3*(lam22*(-(*ka) - lam1) + (*ka)*lam2*lam1 + lam23) - (*ka)*lam22*lam1 - lam24) + eiL3*(*r2)*((*k24)*lam32 + lam3*(-(*k24)*(*k32) - (*ka)*(*k24)) + (*ka)*(*k24)*(*k32))/(lam32*((*ka)*lam1 + lam2*((*ka) + lam1)) + (-(*ka) - lam1 - lam2)*lam33 - (*ka)*lam2*lam1*lam3 + lam34) + (*r2)*(*k24)*(*k32)/(lam2*lam1*lam3)) - (*k42)*(*k24)*(eiL1*(*r2)*((*k23)*lam12 + lam1*(-(*k42)*(*k23) - (*ka)*(*k23)) + (*ka)*(*k42)*(*k23))/(-(*ka)*lam13 + lam2*((*ka)*lam12 - lam13) + lam3*((*ka)*lam12 + lam2*(-(*ka)*lam1 + lam12) - lam13) + lam14) - eiL2*(*r2)*((*k23)*lam22 + lam2*(-(*k42)*(*k23) - (*ka)*(*k23)) + (*ka)*(*k42)*(*k23))/(lam23*((*ka) + lam1) + lam3*(lam22*(-(*ka) - lam1) + (*ka)*lam2*lam1 + lam23) - (*ka)*lam22*lam1 - lam24) + eiL3*(*r2)*((*k23)*lam32 + lam3*(-(*k42)*(*k23) - (*ka)*(*k23)) + (*ka)*(*k42)*(*k23))/(lam32*((*ka)*lam1 + lam2*((*ka) + lam1)) + (-(*ka) - lam1 - lam2)*lam33 - (*ka)*lam2*lam1*lam3 + lam34) + (*r2)*(*k42)*(*k23)/(lam2*lam1*lam3)))/((lam1 - lam3)*(lam1 - lam2)) + eL3*((*k23)*lam3*(-eiL1*(*r2)*(lam1*((*k42)*(*k32) + ((*k32) + (*k42))*(*ka)) + (-(*k32) - (*k42) - (*ka))*lam12 - (*ka)*(*k42)*(*k32) + lam13)/(-(*ka)*lam13 + lam2*((*ka)*lam12 - lam13) + lam3*((*ka)*lam12 + lam2*(-(*ka)*lam1 + lam12) - lam13) + lam14) + eiL2*(*r2)*(lam2*((*k42)*(*k32) + ((*k32) + (*k42))*(*ka)) + (-(*k32) - (*k42) - (*ka))*lam22 - (*ka)*(*k42)*(*k32) + lam23)/(lam23*((*ka) + lam1) + lam3*(lam22*(-(*ka) - lam1) + (*ka)*lam2*lam1 + lam23) - (*ka)*lam22*lam1 - lam24) - eiL3*(*r2)*(lam3*((*k42)*(*k32) + ((*k32) + (*k42))*(*ka)) + (-(*k32) - (*k42) - (*ka))*lam32 - (*ka)*(*k42)*(*k32) + lam33)/(lam32*((*ka)*lam1 + lam2*((*ka) + lam1)) + (-(*ka) - lam1 - lam2)*lam33 - (*ka)*lam2*lam1*lam3 + lam34) + (*r2)*(*k42)*(*k32)/(lam2*lam1*lam3)) - (E4*(*k23)*(-eiL1*(*r2)*(lam1*((*k42)*(*k32) + ((*k32) + (*k42))*(*ka)) + (-(*k32) - (*k42) - (*ka))*lam12 - (*ka)*(*k42)*(*k32) + lam13)/(-(*ka)*lam13 + lam2*((*ka)*lam12 - lam13) + lam3*((*ka)*lam12 + lam2*(-(*ka)*lam1 + lam12) - lam13) + lam14) + eiL2*(*r2)*(lam2*((*k42)*(*k32) + ((*k32) + (*k42))*(*ka)) + (-(*k32) - (*k42) - (*ka))*lam22 - (*ka)*(*k42)*(*k32) + lam23)/(lam23*((*ka) + lam1) + lam3*(lam22*(-(*ka) - lam1) + (*ka)*lam2*lam1 + lam23) - (*ka)*lam22*lam1 - lam24) - eiL3*(*r2)*(lam3*((*k42)*(*k32) + ((*k32) + (*k42))*(*ka)) + (-(*k32) - (*k42) - (*ka))*lam32 - (*ka)*(*k42)*(*k32) + lam33)/(lam32*((*ka)*lam1 + lam2*((*ka) + lam1)) + (-(*ka) - lam1 - lam2)*lam33 - (*ka)*lam2*lam1*lam3 + lam34) + (*r2)*(*k42)*(*k32)/(lam2*lam1*lam3)) + (*k42)*(*k23)*(eiL1*(*r2)*((*k24)*lam12 + lam1*(-(*k24)*(*k32) - (*ka)*(*k24)) + (*ka)*(*k24)*(*k32))/(-(*ka)*lam13 + lam2*((*ka)*lam12 - lam13) + lam3*((*ka)*lam12 + lam2*(-(*ka)*lam1 + lam12) - lam13) + lam14) - eiL2*(*r2)*((*k24)*lam22 + lam2*(-(*k24)*(*k32) - (*ka)*(*k24)) + (*ka)*(*k24)*(*k32))/(lam23*((*ka) + lam1) + lam3*(lam22*(-(*ka) - lam1) + (*ka)*lam2*lam1 + lam23) - (*ka)*lam22*lam1 - lam24) + eiL3*(*r2)*((*k24)*lam32 + lam3*(-(*k24)*(*k32) - (*ka)*(*k24)) + (*ka)*(*k24)*(*k32))/(lam32*((*ka)*lam1 + lam2*((*ka) + lam1)) + (-(*ka) - lam1 - lam2)*lam33 - (*ka)*lam2*lam1*lam3 + lam34) + (*r2)*(*k24)*(*k32)/(lam2*lam1*lam3)) - (*k42)*(*k24)*(eiL1*(*r2)*((*k23)*lam12 + lam1*(-(*k42)*(*k23) - (*ka)*(*k23)) + (*ka)*(*k42)*(*k23))/(-(*ka)*lam13 + lam2*((*ka)*lam12 - lam13) + lam3*((*ka)*lam12 + lam2*(-(*ka)*lam1 + lam12) - lam13) + lam14) - eiL2*(*r2)*((*k23)*lam22 + lam2*(-(*k42)*(*k23) - (*ka)*(*k23)) + (*ka)*(*k42)*(*k23))/(lam23*((*ka) + lam1) + lam3*(lam22*(-(*ka) - lam1) + (*ka)*lam2*lam1 + lam23) - (*ka)*lam22*lam1 - lam24) + eiL3*(*r2)*((*k23)*lam32 + lam3*(-(*k42)*(*k23) - (*ka)*(*k23)) + (*ka)*(*k42)*(*k23))/(lam32*((*ka)*lam1 + lam2*((*ka) + lam1)) + (-(*ka) - lam1 - lam2)*lam33 - (*ka)*lam2*lam1*lam3 + lam34) + (*r2)*(*k42)*(*k23)/(lam2*lam1*lam3))))/((-lam2 + lam3)*(lam1 - lam3)) + eL2*((*k23)*lam2*(-eiL1*(*r2)*(lam1*((*k42)*(*k32) + ((*k32) + (*k42))*(*ka)) + (-(*k32) - (*k42) - (*ka))*lam12 - (*ka)*(*k42)*(*k32) + lam13)/(-(*ka)*lam13 + lam2*((*ka)*lam12 - lam13) + lam3*((*ka)*lam12 + lam2*(-(*ka)*lam1 + lam12) - lam13) + lam14) + eiL2*(*r2)*(lam2*((*k42)*(*k32) + ((*k32) + (*k42))*(*ka)) + (-(*k32) - (*k42) - (*ka))*lam22 - (*ka)*(*k42)*(*k32) + lam23)/(lam23*((*ka) + lam1) + lam3*(lam22*(-(*ka) - lam1) + (*ka)*lam2*lam1 + lam23) - (*ka)*lam22*lam1 - lam24) - eiL3*(*r2)*(lam3*((*k42)*(*k32) + ((*k32) + (*k42))*(*ka)) + (-(*k32) - (*k42) - (*ka))*lam32 - (*ka)*(*k42)*(*k32) + lam33)/(lam32*((*ka)*lam1 + lam2*((*ka) + lam1)) + (-(*ka) - lam1 - lam2)*lam33 - (*ka)*lam2*lam1*lam3 + lam34) + (*r2)*(*k42)*(*k32)/(lam2*lam1*lam3)) - (E4*(*k23)*(-eiL1*(*r2)*(lam1*((*k42)*(*k32) + ((*k32) + (*k42))*(*ka)) + (-(*k32) - (*k42) - (*ka))*lam12 - (*ka)*(*k42)*(*k32) + lam13)/(-(*ka)*lam13 + lam2*((*ka)*lam12 - lam13) + lam3*((*ka)*lam12 + lam2*(-(*ka)*lam1 + lam12) - lam13) + lam14) + eiL2*(*r2)*(lam2*((*k42)*(*k32) + ((*k32) + (*k42))*(*ka)) + (-(*k32) - (*k42) - (*ka))*lam22 - (*ka)*(*k42)*(*k32) + lam23)/(lam23*((*ka) + lam1) + lam3*(lam22*(-(*ka) - lam1) + (*ka)*lam2*lam1 + lam23) - (*ka)*lam22*lam1 - lam24) - eiL3*(*r2)*(lam3*((*k42)*(*k32) + ((*k32) + (*k42))*(*ka)) + (-(*k32) - (*k42) - (*ka))*lam32 - (*ka)*(*k42)*(*k32) + lam33)/(lam32*((*ka)*lam1 + lam2*((*ka) + lam1)) + (-(*ka) - lam1 - lam2)*lam33 - (*ka)*lam2*lam1*lam3 + lam34) + (*r2)*(*k42)*(*k32)/(lam2*lam1*lam3)) + (*k42)*(*k23)*(eiL1*(*r2)*((*k24)*lam12 + lam1*(-(*k24)*(*k32) - (*ka)*(*k24)) + (*ka)*(*k24)*(*k32))/(-(*ka)*lam13 + lam2*((*ka)*lam12 - lam13) + lam3*((*ka)*lam12 + lam2*(-(*ka)*lam1 + lam12) - lam13) + lam14) - eiL2*(*r2)*((*k24)*lam22 + lam2*(-(*k24)*(*k32) - (*ka)*(*k24)) + (*ka)*(*k24)*(*k32))/(lam23*((*ka) + lam1) + lam3*(lam22*(-(*ka) - lam1) + (*ka)*lam2*lam1 + lam23) - (*ka)*lam22*lam1 - lam24) + eiL3*(*r2)*((*k24)*lam32 + lam3*(-(*k24)*(*k32) - (*ka)*(*k24)) + (*ka)*(*k24)*(*k32))/(lam32*((*ka)*lam1 + lam2*((*ka) + lam1)) + (-(*ka) - lam1 - lam2)*lam33 - (*ka)*lam2*lam1*lam3 + lam34) + (*r2)*(*k24)*(*k32)/(lam2*lam1*lam3)) - (*k42)*(*k24)*(eiL1*(*r2)*((*k23)*lam12 + lam1*(-(*k42)*(*k23) - (*ka)*(*k23)) + (*ka)*(*k42)*(*k23))/(-(*ka)*lam13 + lam2*((*ka)*lam12 - lam13) + lam3*((*ka)*lam12 + lam2*(-(*ka)*lam1 + lam12) - lam13) + lam14) - eiL2*(*r2)*((*k23)*lam22 + lam2*(-(*k42)*(*k23) - (*ka)*(*k23)) + (*ka)*(*k42)*(*k23))/(lam23*((*ka) + lam1) + lam3*(lam22*(-(*ka) - lam1) + (*ka)*lam2*lam1 + lam23) - (*ka)*lam22*lam1 - lam24) + eiL3*(*r2)*((*k23)*lam32 + lam3*(-(*k42)*(*k23) - (*ka)*(*k23)) + (*ka)*(*k42)*(*k23))/(lam32*((*ka)*lam1 + lam2*((*ka) + lam1)) + (-(*ka) - lam1 - lam2)*lam33 - (*ka)*lam2*lam1*lam3 + lam34) + (*r2)*(*k42)*(*k23)/(lam2*lam1*lam3))))/((lam2 - lam3)*(lam1 - lam2));
  A4=(eL1*(E3 - lam1)*(E2 - lam1)/((-lam1 + lam3)*(-lam1 + lam2)) + eL2*(E2 - lam2)*(E3 - lam2)/((-lam2 + lam3)*(lam1 - lam2)) + eL3*(E2 - lam3)*(E3 - lam3)/((lam2 - lam3)*(lam1 - lam3)))*(eiL1*(*r2)*((*k24)*lam12 + lam1*(-(*k24)*(*k32) - (*ka)*(*k24)) + (*ka)*(*k24)*(*k32))/(-(*ka)*lam13 + lam2*((*ka)*lam12 - lam13) + lam3*((*ka)*lam12 + lam2*(-(*ka)*lam1 + lam12) - lam13) + lam14) - eiL2*(*r2)*((*k24)*lam22 + lam2*(-(*k24)*(*k32) - (*ka)*(*k24)) + (*ka)*(*k24)*(*k32))/(lam23*((*ka) + lam1) + lam3*(lam22*(-(*ka) - lam1) + (*ka)*lam2*lam1 + lam23) - (*ka)*lam22*lam1 - lam24) + eiL3*(*r2)*((*k24)*lam32 + lam3*(-(*k24)*(*k32) - (*ka)*(*k24)) + (*ka)*(*k24)*(*k32))/(lam32*((*ka)*lam1 + lam2*((*ka) + lam1)) + (-(*ka) - lam1 - lam2)*lam33 - (*ka)*lam2*lam1*lam3 + lam34) + (*r2)*(*k24)*(*k32)/(lam2*lam1*lam3)) + eL1*(E3*(*k24)*(-eiL1*(*r2)*(lam1*((*k42)*(*k32) + ((*k32) + (*k42))*(*ka)) + (-(*k32) - (*k42) - (*ka))*lam12 - (*ka)*(*k42)*(*k32) + lam13)/(-(*ka)*lam13 + lam2*((*ka)*lam12 - lam13) + lam3*((*ka)*lam12 + lam2*(-(*ka)*lam1 + lam12) - lam13) + lam14) + eiL2*(*r2)*(lam2*((*k42)*(*k32) + ((*k32) + (*k42))*(*ka)) + (-(*k32) - (*k42) - (*ka))*lam22 - (*ka)*(*k42)*(*k32) + lam23)/(lam23*((*ka) + lam1) + lam3*(lam22*(-(*ka) - lam1) + (*ka)*lam2*lam1 + lam23) - (*ka)*lam22*lam1 - lam24) - eiL3*(*r2)*(lam3*((*k42)*(*k32) + ((*k32) + (*k42))*(*ka)) + (-(*k32) - (*k42) - (*ka))*lam32 - (*ka)*(*k42)*(*k32) + lam33)/(lam32*((*ka)*lam1 + lam2*((*ka) + lam1)) + (-(*ka) - lam1 - lam2)*lam33 - (*ka)*lam2*lam1*lam3 + lam34) + (*r2)*(*k42)*(*k32)/(lam2*lam1*lam3)) - (*k23)*(*k32)*(eiL1*(*r2)*((*k24)*lam12 + lam1*(-(*k24)*(*k32) - (*ka)*(*k24)) + (*ka)*(*k24)*(*k32))/(-(*ka)*lam13 + lam2*((*ka)*lam12 - lam13) + lam3*((*ka)*lam12 + lam2*(-(*ka)*lam1 + lam12) - lam13) + lam14) - eiL2*(*r2)*((*k24)*lam22 + lam2*(-(*k24)*(*k32) - (*ka)*(*k24)) + (*ka)*(*k24)*(*k32))/(lam23*((*ka) + lam1) + lam3*(lam22*(-(*ka) - lam1) + (*ka)*lam2*lam1 + lam23) - (*ka)*lam22*lam1 - lam24) + eiL3*(*r2)*((*k24)*lam32 + lam3*(-(*k24)*(*k32) - (*ka)*(*k24)) + (*ka)*(*k24)*(*k32))/(lam32*((*ka)*lam1 + lam2*((*ka) + lam1)) + (-(*ka) - lam1 - lam2)*lam33 - (*ka)*lam2*lam1*lam3 + lam34) + (*r2)*(*k24)*(*k32)/(lam2*lam1*lam3)) + (*k24)*(*k32)*(eiL1*(*r2)*((*k23)*lam12 + lam1*(-(*k42)*(*k23) - (*ka)*(*k23)) + (*ka)*(*k42)*(*k23))/(-(*ka)*lam13 + lam2*((*ka)*lam12 - lam13) + lam3*((*ka)*lam12 + lam2*(-(*ka)*lam1 + lam12) - lam13) + lam14) - eiL2*(*r2)*((*k23)*lam22 + lam2*(-(*k42)*(*k23) - (*ka)*(*k23)) + (*ka)*(*k42)*(*k23))/(lam23*((*ka) + lam1) + lam3*(lam22*(-(*ka) - lam1) + (*ka)*lam2*lam1 + lam23) - (*ka)*lam22*lam1 - lam24) + eiL3*(*r2)*((*k23)*lam32 + lam3*(-(*k42)*(*k23) - (*ka)*(*k23)) + (*ka)*(*k42)*(*k23))/(lam32*((*ka)*lam1 + lam2*((*ka) + lam1)) + (-(*ka) - lam1 - lam2)*lam33 - (*ka)*lam2*lam1*lam3 + lam34) + (*r2)*(*k42)*(*k23)/(lam2*lam1*lam3)) - (*k24)*lam1*(-eiL1*(*r2)*(lam1*((*k42)*(*k32) + ((*k32) + (*k42))*(*ka)) + (-(*k32) - (*k42) - (*ka))*lam12 - (*ka)*(*k42)*(*k32) + lam13)/(-(*ka)*lam13 + lam2*((*ka)*lam12 - lam13) + lam3*((*ka)*lam12 + lam2*(-(*ka)*lam1 + lam12) - lam13) + lam14) + eiL2*(*r2)*(lam2*((*k42)*(*k32) + ((*k32) + (*k42))*(*ka)) + (-(*k32) - (*k42) - (*ka))*lam22 - (*ka)*(*k42)*(*k32) + lam23)/(lam23*((*ka) + lam1) + lam3*(lam22*(-(*ka) - lam1) + (*ka)*lam2*lam1 + lam23) - (*ka)*lam22*lam1 - lam24) - eiL3*(*r2)*(lam3*((*k42)*(*k32) + ((*k32) + (*k42))*(*ka)) + (-(*k32) - (*k42) - (*ka))*lam32 - (*ka)*(*k42)*(*k32) + lam33)/(lam32*((*ka)*lam1 + lam2*((*ka) + lam1)) + (-(*ka) - lam1 - lam2)*lam33 - (*ka)*lam2*lam1*lam3 + lam34) + (*r2)*(*k42)*(*k32)/(lam2*lam1*lam3)))/((lam1 - lam3)*(lam1 - lam2)) + eL3*((*k24)*lam3*(-eiL1*(*r2)*(lam1*((*k42)*(*k32) + ((*k32) + (*k42))*(*ka)) + (-(*k32) - (*k42) - (*ka))*lam12 - (*ka)*(*k42)*(*k32) + lam13)/(-(*ka)*lam13 + lam2*((*ka)*lam12 - lam13) + lam3*((*ka)*lam12 + lam2*(-(*ka)*lam1 + lam12) - lam13) + lam14) + eiL2*(*r2)*(lam2*((*k42)*(*k32) + ((*k32) + (*k42))*(*ka)) + (-(*k32) - (*k42) - (*ka))*lam22 - (*ka)*(*k42)*(*k32) + lam23)/(lam23*((*ka) + lam1) + lam3*(lam22*(-(*ka) - lam1) + (*ka)*lam2*lam1 + lam23) - (*ka)*lam22*lam1 - lam24) - eiL3*(*r2)*(lam3*((*k42)*(*k32) + ((*k32) + (*k42))*(*ka)) + (-(*k32) - (*k42) - (*ka))*lam32 - (*ka)*(*k42)*(*k32) + lam33)/(lam32*((*ka)*lam1 + lam2*((*ka) + lam1)) + (-(*ka) - lam1 - lam2)*lam33 - (*ka)*lam2*lam1*lam3 + lam34) + (*r2)*(*k42)*(*k32)/(lam2*lam1*lam3)) - (E3*(*k24)*(-eiL1*(*r2)*(lam1*((*k42)*(*k32) + ((*k32) + (*k42))*(*ka)) + (-(*k32) - (*k42) - (*ka))*lam12 - (*ka)*(*k42)*(*k32) + lam13)/(-(*ka)*lam13 + lam2*((*ka)*lam12 - lam13) + lam3*((*ka)*lam12 + lam2*(-(*ka)*lam1 + lam12) - lam13) + lam14) + eiL2*(*r2)*(lam2*((*k42)*(*k32) + ((*k32) + (*k42))*(*ka)) + (-(*k32) - (*k42) - (*ka))*lam22 - (*ka)*(*k42)*(*k32) + lam23)/(lam23*((*ka) + lam1) + lam3*(lam22*(-(*ka) - lam1) + (*ka)*lam2*lam1 + lam23) - (*ka)*lam22*lam1 - lam24) - eiL3*(*r2)*(lam3*((*k42)*(*k32) + ((*k32) + (*k42))*(*ka)) + (-(*k32) - (*k42) - (*ka))*lam32 - (*ka)*(*k42)*(*k32) + lam33)/(lam32*((*ka)*lam1 + lam2*((*ka) + lam1)) + (-(*ka) - lam1 - lam2)*lam33 - (*ka)*lam2*lam1*lam3 + lam34) + (*r2)*(*k42)*(*k32)/(lam2*lam1*lam3)) - (*k23)*(*k32)*(eiL1*(*r2)*((*k24)*lam12 + lam1*(-(*k24)*(*k32) - (*ka)*(*k24)) + (*ka)*(*k24)*(*k32))/(-(*ka)*lam13 + lam2*((*ka)*lam12 - lam13) + lam3*((*ka)*lam12 + lam2*(-(*ka)*lam1 + lam12) - lam13) + lam14) - eiL2*(*r2)*((*k24)*lam22 + lam2*(-(*k24)*(*k32) - (*ka)*(*k24)) + (*ka)*(*k24)*(*k32))/(lam23*((*ka) + lam1) + lam3*(lam22*(-(*ka) - lam1) + (*ka)*lam2*lam1 + lam23) - (*ka)*lam22*lam1 - lam24) + eiL3*(*r2)*((*k24)*lam32 + lam3*(-(*k24)*(*k32) - (*ka)*(*k24)) + (*ka)*(*k24)*(*k32))/(lam32*((*ka)*lam1 + lam2*((*ka) + lam1)) + (-(*ka) - lam1 - lam2)*lam33 - (*ka)*lam2*lam1*lam3 + lam34) + (*r2)*(*k24)*(*k32)/(lam2*lam1*lam3)) + (*k24)*(*k32)*(eiL1*(*r2)*((*k23)*lam12 + lam1*(-(*k42)*(*k23) - (*ka)*(*k23)) + (*ka)*(*k42)*(*k23))/(-(*ka)*lam13 + lam2*((*ka)*lam12 - lam13) + lam3*((*ka)*lam12 + lam2*(-(*ka)*lam1 + lam12) - lam13) + lam14) - eiL2*(*r2)*((*k23)*lam22 + lam2*(-(*k42)*(*k23) - (*ka)*(*k23)) + (*ka)*(*k42)*(*k23))/(lam23*((*ka) + lam1) + lam3*(lam22*(-(*ka) - lam1) + (*ka)*lam2*lam1 + lam23) - (*ka)*lam22*lam1 - lam24) + eiL3*(*r2)*((*k23)*lam32 + lam3*(-(*k42)*(*k23) - (*ka)*(*k23)) + (*ka)*(*k42)*(*k23))/(lam32*((*ka)*lam1 + lam2*((*ka) + lam1)) + (-(*ka) - lam1 - lam2)*lam33 - (*ka)*lam2*lam1*lam3 + lam34) + (*r2)*(*k42)*(*k23)/(lam2*lam1*lam3))))/((-lam2 + lam3)*(lam1 - lam3)) + eL2*((*k24)*lam2*(-eiL1*(*r2)*(lam1*((*k42)*(*k32) + ((*k32) + (*k42))*(*ka)) + (-(*k32) - (*k42) - (*ka))*lam12 - (*ka)*(*k42)*(*k32) + lam13)/(-(*ka)*lam13 + lam2*((*ka)*lam12 - lam13) + lam3*((*ka)*lam12 + lam2*(-(*ka)*lam1 + lam12) - lam13) + lam14) + eiL2*(*r2)*(lam2*((*k42)*(*k32) + ((*k32) + (*k42))*(*ka)) + (-(*k32) - (*k42) - (*ka))*lam22 - (*ka)*(*k42)*(*k32) + lam23)/(lam23*((*ka) + lam1) + lam3*(lam22*(-(*ka) - lam1) + (*ka)*lam2*lam1 + lam23) - (*ka)*lam22*lam1 - lam24) - eiL3*(*r2)*(lam3*((*k42)*(*k32) + ((*k32) + (*k42))*(*ka)) + (-(*k32) - (*k42) - (*ka))*lam32 - (*ka)*(*k42)*(*k32) + lam33)/(lam32*((*ka)*lam1 + lam2*((*ka) + lam1)) + (-(*ka) - lam1 - lam2)*lam33 - (*ka)*lam2*lam1*lam3 + lam34) + (*r2)*(*k42)*(*k32)/(lam2*lam1*lam3)) - (E3*(*k24)*(-eiL1*(*r2)*(lam1*((*k42)*(*k32) + ((*k32) + (*k42))*(*ka)) + (-(*k32) - (*k42) - (*ka))*lam12 - (*ka)*(*k42)*(*k32) + lam13)/(-(*ka)*lam13 + lam2*((*ka)*lam12 - lam13) + lam3*((*ka)*lam12 + lam2*(-(*ka)*lam1 + lam12) - lam13) + lam14) + eiL2*(*r2)*(lam2*((*k42)*(*k32) + ((*k32) + (*k42))*(*ka)) + (-(*k32) - (*k42) - (*ka))*lam22 - (*ka)*(*k42)*(*k32) + lam23)/(lam23*((*ka) + lam1) + lam3*(lam22*(-(*ka) - lam1) + (*ka)*lam2*lam1 + lam23) - (*ka)*lam22*lam1 - lam24) - eiL3*(*r2)*(lam3*((*k42)*(*k32) + ((*k32) + (*k42))*(*ka)) + (-(*k32) - (*k42) - (*ka))*lam32 - (*ka)*(*k42)*(*k32) + lam33)/(lam32*((*ka)*lam1 + lam2*((*ka) + lam1)) + (-(*ka) - lam1 - lam2)*lam33 - (*ka)*lam2*lam1*lam3 + lam34) + (*r2)*(*k42)*(*k32)/(lam2*lam1*lam3)) - (*k23)*(*k32)*(eiL1*(*r2)*((*k24)*lam12 + lam1*(-(*k24)*(*k32) - (*ka)*(*k24)) + (*ka)*(*k24)*(*k32))/(-(*ka)*lam13 + lam2*((*ka)*lam12 - lam13) + lam3*((*ka)*lam12 + lam2*(-(*ka)*lam1 + lam12) - lam13) + lam14) - eiL2*(*r2)*((*k24)*lam22 + lam2*(-(*k24)*(*k32) - (*ka)*(*k24)) + (*ka)*(*k24)*(*k32))/(lam23*((*ka) + lam1) + lam3*(lam22*(-(*ka) - lam1) + (*ka)*lam2*lam1 + lam23) - (*ka)*lam22*lam1 - lam24) + eiL3*(*r2)*((*k24)*lam32 + lam3*(-(*k24)*(*k32) - (*ka)*(*k24)) + (*ka)*(*k24)*(*k32))/(lam32*((*ka)*lam1 + lam2*((*ka) + lam1)) + (-(*ka) - lam1 - lam2)*lam33 - (*ka)*lam2*lam1*lam3 + lam34) + (*r2)*(*k24)*(*k32)/(lam2*lam1*lam3)) + (*k24)*(*k32)*(eiL1*(*r2)*((*k23)*lam12 + lam1*(-(*k42)*(*k23) - (*ka)*(*k23)) + (*ka)*(*k42)*(*k23))/(-(*ka)*lam13 + lam2*((*ka)*lam12 - lam13) + lam3*((*ka)*lam12 + lam2*(-(*ka)*lam1 + lam12) - lam13) + lam14) - eiL2*(*r2)*((*k23)*lam22 + lam2*(-(*k42)*(*k23) - (*ka)*(*k23)) + (*ka)*(*k42)*(*k23))/(lam23*((*ka) + lam1) + lam3*(lam22*(-(*ka) - lam1) + (*ka)*lam2*lam1 + lam23) - (*ka)*lam22*lam1 - lam24) + eiL3*(*r2)*((*k23)*lam32 + lam3*(-(*k42)*(*k23) - (*ka)*(*k23)) + (*ka)*(*k42)*(*k23))/(lam32*((*ka)*lam1 + lam2*((*ka) + lam1)) + (-(*ka) - lam1 - lam2)*lam33 - (*ka)*lam2*lam1*lam3 + lam34) + (*r2)*(*k42)*(*k23)/(lam2*lam1*lam3))))/((lam2 - lam3)*(lam1 - lam2));
}

static inline void threeCmtKaRate(double *A, double *Alast,
				  double *t,
				  double *b1, double *b2,
				  double *r1, double *r2,
				  double *ka, double *k20,
				  double *k23, double *k32,
				  double *k24, double *k42) {
  double E2 =  (*k20)+ (*k23) + (*k24);
  double j = (*k23)+(*k20)+(*k32)+(*k42)+(*k24);
  double k = (*k23)*(*k42)+(*k20)*(*k32)+(*k20)*(*k42)+(*k32)*(*k42)+(*k24)*(*k32);
  double l = (*k20)*(*k32)*(*k42);

  double m = 0.3333333333333333*(3.0*k - j*j);
  double n = 0.03703703703703703*(2.0*j*j*j - 9.0*j*k + 27.0*l);
  double Q = 0.25*n*n + 0.03703703703703703*m*m*m;

  double alpha = sqrt(-Q);
  double beta = -0.5*n;
  double rho=sqrt(beta*beta+alpha*alpha);
  double theta = atan2(alpha,beta);
  double ct3 = cos(0.3333333333333333*theta);
  double rho3 = R_pow(rho,0.3333333333333333);
  double st3 = 1.732050807568877193177*sin(0.3333333333333333*theta);
  double j3 = 0.3333333333333333*j;
  double lam1 = j3  + rho3*(ct3 + st3);
  double lam2 = j3 + rho3*(ct3 - st3);
  double lam3 = j3 -(2.0*rho3*ct3);
  double eKa = exp(-(*ka)*(*t));
  A1 = (*b1)+ (*r1)/(*ka)-(((*r1)-A1last*(*ka))*eKa)/(*ka);
  
  double lam12 = lam1*lam1;
  double lam13 = lam12*lam1;
  double lam14 = lam13*lam1;
  
  double lam22 = lam2*lam2;
  double lam23 = lam22*lam2;
  double lam24 = lam23*lam2;
  
  double lam32 = lam3*lam3;
  double lam33 = lam32*lam3;
  double lam34 = lam33*lam3;

  double ka2 = (*ka)*(*ka);
  double ka3 = ka2*(*ka);
  
  double a21 = (((lam33+(-(*ka)-(*k42)-(*k32))*lam32+(((*k42)+(*k32))*(*ka)+(*k32)*(*k42))*lam3-(*k32)*(*k42)*(*ka))*(*r2)+(-(*ka)*lam32+((*k42)+(*k32))*(*ka)*lam3-(*k32)*(*k42)*(*ka))*(*r1)-A2last*lam34+((A2last+A1last)*(*ka)+(A4last+A2last)*(*k42)+(A3last+A2last)*(*k32))*lam33+(((-A4last-A2last-A1last)*(*k42)+(-A3last-A2last-A1last)*(*k32))*(*ka)+(-A4last-A3last-A2last)*(*k32)*(*k42))*lam32+(A4last+A3last+A2last+A1last)*(*k32)*(*k42)*(*ka)*lam3)*exp(-lam3*(*t)))/(lam34+(-lam2-lam1-(*ka))*lam33+((lam1+(*ka))*lam2+(*ka)*lam1)*lam32-(*ka)*lam1*lam2*lam3);
  double a22 = (((lam23+(-(*ka)-(*k42)-(*k32))*lam22+(((*k42)+(*k32))*(*ka)+(*k32)*(*k42))*lam2-(*k32)*(*k42)*(*ka))*(*r2)+(-(*ka)*lam22+((*k42)+(*k32))*(*ka)*lam2-(*k32)*(*k42)*(*ka))*(*r1)-A2last*lam24+((A2last+A1last)*(*ka)+(A4last+A2last)*(*k42)+(A3last+A2last)*(*k32))*lam23+(((-A4last-A2last-A1last)*(*k42)+(-A3last-A2last-A1last)*(*k32))*(*ka)+(-A4last-A3last-A2last)*(*k32)*(*k42))*lam22+(A4last+A3last+A2last+A1last)*(*k32)*(*k42)*(*ka)*lam2)*exp(-lam2*(*t)))/((lam23+(-lam1-(*ka))*lam22+(*ka)*lam1*lam2)*lam3-lam24+(lam1+(*ka))*lam23-(*ka)*lam1*lam22);
  double a23 = (((lam13+(-(*ka)-(*k42)-(*k32))*lam12+(((*k42)+(*k32))*(*ka)+(*k32)*(*k42))*lam1-(*k32)*(*k42)*(*ka))*(*r2)+(-(*ka)*lam12+((*k42)+(*k32))*(*ka)*lam1-(*k32)*(*k42)*(*ka))*(*r1)-A2last*lam14+((A2last+A1last)*(*ka)+(A4last+A2last)*(*k42)+(A3last+A2last)*(*k32))*lam13+(((-A4last-A2last-A1last)*(*k42)+(-A3last-A2last-A1last)*(*k32))*(*ka)+(-A4last-A3last-A2last)*(*k32)*(*k42))*lam12+(A4last+A3last+A2last+A1last)*(*k32)*(*k42)*(*ka)*lam1)*exp(-lam1*(*t)))/(((lam12-(*ka)*lam1)*lam2-lam13+(*ka)*lam12)*lam3+((*ka)*lam12-lam13)*lam2+lam14-(*ka)*lam13);
  double a24 = (((ka2+(-(*k42)-(*k32))*(*ka)+(*k32)*(*k42))*(*r1)-A1last*ka3+(A1last*(*k42)+A1last*(*k32))*ka2-A1last*(*k32)*(*k42)*(*ka))*exp(-(*ka)*(*t)))/(((lam1-(*ka))*lam2-(*ka)*lam1+ka2)*lam3+(ka2-(*ka)*lam1)*lam2+ka2*lam1-ka3);
  double a25 = ((*k32)*(*k42)*(*r2)+(*k32)*(*k42)*(*r1))/(lam1*lam2*lam3);
  A2 = (*b2)-a21+a22-a23-a24+a25;
  double a31 = ((((*k23)*lam32+(-(*k23)*(*ka)-(*k23)*(*k42))*lam3+(*k23)*(*k42)*(*ka))*(*r2)+((*k23)*(*k42)*(*ka)-(*k23)*(*ka)*lam3)*(*r1)+A3last*lam34+(-A3last*(*ka)-A3last*(*k42)-A2last*(*k23)-A3last*E2)*lam33+((A3last*(*k42)+(A2last+A1last)*(*k23)+A3last*E2)*(*ka)+(-A3last*(*k24)+(A4last+A2last)*(*k23)+A3last*E2)*(*k42))*lam32+(A3last*(*k24)+(-A4last-A2last-A1last)*(*k23)-A3last*E2)*(*k42)*(*ka)*lam3)*exp(-lam3*(*t)))/(lam34+(-lam2-lam1-(*ka))*lam33+((lam1+(*ka))*lam2+(*ka)*lam1)*lam32-(*ka)*lam1*lam2*lam3);
  double a32 = ((((*k23)*lam22+(-(*k23)*(*ka)-(*k23)*(*k42))*lam2+(*k23)*(*k42)*(*ka))*(*r2)+((*k23)*(*k42)*(*ka)-(*k23)*(*ka)*lam2)*(*r1)+A3last*lam24+(-A3last*(*ka)-A3last*(*k42)-A2last*(*k23)-A3last*E2)*lam23+((A3last*(*k42)+(A2last+A1last)*(*k23)+A3last*E2)*(*ka)+(-A3last*(*k24)+(A4last+A2last)*(*k23)+A3last*E2)*(*k42))*lam22+(A3last*(*k24)+(-A4last-A2last-A1last)*(*k23)-A3last*E2)*(*k42)*(*ka)*lam2)*exp(-lam2*(*t)))/((lam23+(-lam1-(*ka))*lam22+(*ka)*lam1*lam2)*lam3-lam24+(lam1+(*ka))*lam23-(*ka)*lam1*lam22);
  double a33 = ((((*k23)*lam12+(-(*k23)*(*ka)-(*k23)*(*k42))*lam1+(*k23)*(*k42)*(*ka))*(*r2)+((*k23)*(*k42)*(*ka)-(*k23)*(*ka)*lam1)*(*r1)+A3last*lam14+(-A3last*(*ka)-A3last*(*k42)-A2last*(*k23)-A3last*E2)*lam13+((A3last*(*k42)+(A2last+A1last)*(*k23)+A3last*E2)*(*ka)+(-A3last*(*k24)+(A4last+A2last)*(*k23)+A3last*E2)*(*k42))*lam12+(A3last*(*k24)+(-A4last-A2last-A1last)*(*k23)-A3last*E2)*(*k42)*(*ka)*lam1)*exp(-lam1*(*t)))/(((lam12-(*ka)*lam1)*lam2-lam13+(*ka)*lam12)*lam3+((*ka)*lam12-lam13)*lam2+lam14-(*ka)*lam13);
  double a34 = ((((*k23)*(*ka)-(*k23)*(*k42))*(*r1)-A1last*(*k23)*ka2+A1last*(*k23)*(*k42)*(*ka))*exp(-(*ka)*(*t)))/(((lam1-(*ka))*lam2-(*ka)*lam1+ka2)*lam3+(ka2-(*ka)*lam1)*lam2+ka2*lam1-ka3);
  double a35 = ((*k23)*(*k42)*(*r2)+(*k23)*(*k42)*(*r1))/(lam1*lam2*lam3);
  A3=a31-a32+a33+a34+a35;
  double a41 = ((((*k24)*lam32+(-(*k24)*(*ka)-(*k24)*(*k32))*lam3+(*k24)*(*k32)*(*ka))*(*r2)+((*k24)*(*k32)*(*ka)-(*k24)*(*ka)*lam3)*(*r1)+A4last*lam34+(-A4last*(*ka)-A4last*(*k32)-A2last*(*k24)-A4last*E2)*lam33+((A4last*(*k32)+(A2last+A1last)*(*k24)+A4last*E2)*(*ka)+((A3last+A2last)*(*k24)-A4last*(*k23)+A4last*E2)*(*k32))*lam32+((-A3last-A2last-A1last)*(*k24)+A4last*(*k23)-A4last*E2)*(*k32)*(*ka)*lam3)*exp(-lam3*(*t)))/(lam34+(-lam2-lam1-(*ka))*lam33+((lam1+(*ka))*lam2+(*ka)*lam1)*lam32-(*ka)*lam1*lam2*lam3);
  double a42 = ((((*k24)*lam22+(-(*k24)*(*ka)-(*k24)*(*k32))*lam2+(*k24)*(*k32)*(*ka))*(*r2)+((*k24)*(*k32)*(*ka)-(*k24)*(*ka)*lam2)*(*r1)+A4last*lam24+(-A4last*(*ka)-A4last*(*k32)-A2last*(*k24)-A4last*E2)*lam23+((A4last*(*k32)+(A2last+A1last)*(*k24)+A4last*E2)*(*ka)+((A3last+A2last)*(*k24)-A4last*(*k23)+A4last*E2)*(*k32))*lam22+((-A3last-A2last-A1last)*(*k24)+A4last*(*k23)-A4last*E2)*(*k32)*(*ka)*lam2)*exp(-lam2*(*t)))/((lam23+(-lam1-(*ka))*lam22+(*ka)*lam1*lam2)*lam3-lam24+(lam1+(*ka))*lam23-(*ka)*lam1*lam22);
  double a43 = ((((*k24)*lam12+(-(*k24)*(*ka)-(*k24)*(*k32))*lam1+(*k24)*(*k32)*(*ka))*(*r2)+((*k24)*(*k32)*(*ka)-(*k24)*(*ka)*lam1)*(*r1)+A4last*lam14+(-A4last*(*ka)-A4last*(*k32)-A2last*(*k24)-A4last*E2)*lam13+((A4last*(*k32)+(A2last+A1last)*(*k24)+A4last*E2)*(*ka)+((A3last+A2last)*(*k24)-A4last*(*k23)+A4last*E2)*(*k32))*lam12+((-A3last-A2last-A1last)*(*k24)+A4last*(*k23)-A4last*E2)*(*k32)*(*ka)*lam1)*exp(-lam1*(*t)))/(((lam12-(*ka)*lam1)*lam2-lam13+(*ka)*lam12)*lam3+((*ka)*lam12-lam13)*lam2+lam14-(*ka)*lam13);
  double a44 = ((((*k24)*(*ka)-(*k24)*(*k32))*(*r1)-A1last*(*k24)*ka2+A1last*(*k24)*(*k32)*(*ka))*exp(-(*ka)*(*t)))/(((lam1-(*ka))*lam2-(*ka)*lam1+ka2)*lam3+(ka2-(*ka)*lam1)*lam2+ka2*lam1-ka3);
  double a45 = ((*k24)*(*k32)*(*r2)+(*k24)*(*k32)*(*r1))/(lam1*lam2*lam3);
  A4=a41-a42+a43+a44+a45;
}


////////////////////////////////////////////////////////////////////////////////
// 1-3 oral absorption with rates.
// Adapted from Richard Upton's maxima notebooks and supplementary material from
// Abuhelwa2015
////////////////////////////////////////////////////////////////////////////////

static inline void oneCmtKaSSb1(double *A, double *tau,
				double *b1, double *ka, double *k20) {
  double eKa = 1.0/(1.0-exp(-(*tau)*(*ka)));
  double eK =  1.0/(1.0-exp(-(*tau)*(*k20)));
  A1=eKa*(*b1);
  A2=(*ka)*(*b1)*(eK - eKa)/(-(*k20) + (*ka));
}

static inline void oneCmtKaSSb2(double *A, double *tau,
				double *b2, double *ka, double *k20) {
  double eK =  1.0/(1.0-exp(-(*tau)*(*k20)));
  A1=0.0;
  A2=eK*(*b2);
}

static inline void oneCmtKa(double *A, double *Alast,
			    double *t, double *b1, double *b2,
			    double *ka, double *k20) {
  double rx_expr_0=exp(-(*t)*(*ka));
  A1=A1last*rx_expr_0+(*b1);
  double rx_expr_1=exp(-(*t)*(*k20));
  A2=A1last*(*ka)/((*ka)-(*k20))*(rx_expr_1-rx_expr_0)+A2last*rx_expr_1+(*b2);
}

static inline void twoCmtKaSSb1(double *A, double *tau, double *b1,
				double *ka, double *k20,
				double *k23, double *k32) {
  double E2 = (*k20)+(*k23);
  double E3 = (*k32);
  double e2e3 = E2+E3;
  double s = sqrt(e2e3*e2e3-4*(E2*E3-(*k23)*(*k32)));

  double lambda1 = 0.5*(e2e3+s);
  double lambda2 = 0.5*(e2e3-s);
  double eKa=1.0/(1.0-exp(-(*tau)*(*ka)));
  double eL1=1.0/(1.0-exp(-(*tau)*lambda1));
  double eL2=1.0/(1.0-exp(-(*tau)*lambda2));
  A1=eKa*(*b1);
  A2=(*ka)*(*b1)*(eL1*(E3 - lambda1)/((-lambda1 + lambda2)*((*ka) - lambda1)) + eL2*(E3 - lambda2)/((lambda1 - lambda2)*((*ka) - lambda2)) + eKa*(E3 - (*ka))/((-(*ka) + lambda2)*(-(*ka) + lambda1)));
  A3=(*ka)*(*b1)*(*k23)*(eL1/((-lambda1 + lambda2)*((*ka) - lambda1)) + eL2/((lambda1 - lambda2)*((*ka) - lambda2)) + eKa/((-(*ka) + lambda2)*(-(*ka) + lambda1)));
}

static inline void twoCmtKaSSb2(double *A, double *tau, double *b2,
				double *ka, double *k20,
				double *k23, double *k32) {
  double E2 = (*k20)+(*k23);
  double E3 = (*k32);
  double e2e3 = E2+E3;
  double s = sqrt(e2e3*e2e3-4*(E2*E3-(*k23)*(*k32)));

  double lambda1 = 0.5*(e2e3+s);
  double lambda2 = 0.5*(e2e3-s);
  double eL1=1.0/(1.0-exp(-(*tau)*lambda1));
  double eL2=1.0/(1.0-exp(-(*tau)*lambda2));

  A1=0.0;
  A2=(eL1*((*b2)*E3 - (*b2)*lambda1) - eL2*((*b2)*E3 - (*b2)*lambda2))/(-lambda1 + lambda2);
  A3=(eL1*(*b2)*(*k23) - eL2*(*b2)*(*k23))/(-lambda1 + lambda2);
}

static inline void twoCmtKa(double *A, double *Alast, double *t,
			    double *b1, double *b2,
			    double *ka, double *k20,
			    double *k23, double *k32) {
  double rxe2=exp(-(*t)*(*ka));
  A1=(*b1)+rxe2*A1last;
  double rxe0=(*k23)+(*k32);
  double rxe1=(*k23)+(*k20);
  double rxe3=(*k32)*A2last;
  double rxe4=(*k32)*A3last;
  double rxe6=rxe0+(*k20);
  double rxe7=(rxe1)*(*k32);
  double rxe8=rxe6*rxe6;
  double rxe10=sqrt(-4*(-(*k23)*(*k32)+rxe7)+rxe8);
  A2=(*b2)+(-exp(-0.5*(*t)*(rxe6-rxe10))*(-0.5*A2last*(rxe6-rxe10)+rxe3+rxe4)+exp(-0.5*(*t)*(rxe6+rxe10))*(-0.5*A2last*(rxe6+rxe10)+rxe3+rxe4))/(0.5*(rxe6-rxe10)-0.5*(rxe6+rxe10))+(*ka)*(rxe2*((*k32)-(*ka))/((-(*ka)+0.5*(rxe6-rxe10))*(-(*ka)+0.5*(rxe6+rxe10)))+exp(-0.5*(*t)*(rxe6-rxe10))*((*k32)-0.5*(rxe6-rxe10))/((-0.5*(rxe6-rxe10)+0.5*(rxe6+rxe10))*((*ka)-0.5*(rxe6-rxe10)))+exp(-0.5*(*t)*(rxe6+rxe10))*((*k32)-0.5*(rxe6+rxe10))/((0.5*(rxe6-rxe10)-0.5*(rxe6+rxe10))*((*ka)-0.5*(rxe6+rxe10))))*A1last;
  double rxe5=(*k23)*A2last;
  double rxe9=(rxe1)*A3last;
  A3=(-exp(-0.5*(*t)*(rxe6-rxe10))*(-0.5*A3last*(rxe6-rxe10)+rxe5+rxe9)+exp(-0.5*(*t)*(rxe6+rxe10))*(-0.5*A3last*(rxe6+rxe10)+rxe5+rxe9))/(0.5*(rxe6-rxe10)-0.5*(rxe6+rxe10))+(*ka)*(*k23)*A1last*(rxe2/((-(*ka)+0.5*(rxe6-rxe10))*(-(*ka)+0.5*(rxe6+rxe10)))+exp(-0.5*(*t)*(rxe6-rxe10))/((-0.5*(rxe6-rxe10)+0.5*(rxe6+rxe10))*((*ka)-0.5*(rxe6-rxe10)))+exp(-0.5*(*t)*(rxe6+rxe10))/((0.5*(rxe6-rxe10)-0.5*(rxe6+rxe10))*((*ka)-0.5*(rxe6+rxe10))));
}


static inline void threeCmtKaSSb1(double *A, double *tau, double *b1, 
				  double *KA, double *k20,
				  double *k23, double *k32,
				  double *k24, double *k42){
  double E2 = (*k20)+(*k23)+(*k24);
  double E3 = (*k32);
  double E4 = (*k42);

  double a = E2+E3+E4;
  double b = E2*E3+E4*(E2+E3)-(*k23)*(*k32)-(*k24)*(*k42);
  double c = E2*E3*E4-E4*(*k23)*(*k32)-E3*(*k24)*(*k42);

  double a2 = a*a;
  double m = 0.333333333333333*(3.0*b - a2);
  double n = 0.03703703703703703*(2.0*a2*a - 9.0*a*b + 27.0*c);
  double Q = 0.25*(n*n) + 0.03703703703703703*(m*m*m);

  double alpha = sqrt(-Q);
  double beta = -0.5*n;
  double gamma = sqrt(beta*beta+alpha*alpha);
  double theta = atan2(alpha,beta);
  double theta3 = 0.333333333333333*theta;
  double ctheta3 = cos(theta3);
  double stheta3 = 1.7320508075688771932*sin(theta3);
  double gamma3 = R_pow(gamma,0.333333333333333);
  
  double lambda1 = 0.333333333333333*a + gamma3*(ctheta3 + stheta3);
  double lambda2 = 0.333333333333333*a + gamma3*(ctheta3 -stheta3);
  double lambda3 = 0.333333333333333*a -(2.0*gamma3*ctheta3);

  double eKa = 1.0/(1.0-exp(-(*tau)*(*KA)));
  double eL1 = 1.0/(1.0-exp(-(*tau)*lambda1));
  double eL2 = 1.0/(1.0-exp(-(*tau)*lambda2));
  double eL3 = 1.0/(1.0-exp(-(*tau)*lambda3));
  
  A1=eKa*(*b1);
  A2=(*KA)*(*b1)*(eL1*(E3 - lambda1)*(E4 - lambda1)/((-lambda1 + lambda3)*(-lambda1 + lambda2)*((*KA) - lambda1)) + eL2*(E3 - lambda2)*(E4 - lambda2)/((lambda1 - lambda2)*(-lambda2 + lambda3)*((*KA) - lambda2)) + eL3*(E3 - lambda3)*(E4 - lambda3)/((lambda1 - lambda3)*(lambda2 - lambda3)*((*KA) - lambda3)) + eKa*(E3 - (*KA))*(E4 - (*KA))/((-(*KA) + lambda1)*(-(*KA) + lambda3)*(-(*KA) + lambda2)));
  A3=(*KA)*(*b1)*(*k23)*(eL1*(E4 - lambda1)/((-lambda1 + lambda3)*(-lambda1 + lambda2)*((*KA) - lambda1)) + eL2*(E4 - lambda2)/((lambda1 - lambda2)*(-lambda2 + lambda3)*((*KA) - lambda2)) + eL3*(E4 - lambda3)/((lambda1 - lambda3)*(lambda2 - lambda3)*((*KA) - lambda3)) + eKa*(E4 - (*KA))/((-(*KA) + lambda1)*(-(*KA) + lambda3)*(-(*KA) + lambda2)));
  A4=(*KA)*(*b1)*(*k24)*(eL1*(E3 - lambda1)/((-lambda1 + lambda3)*(-lambda1 + lambda2)*((*KA) - lambda1)) + eL2*(E3 - lambda2)/((lambda1 - lambda2)*(-lambda2 + lambda3)*((*KA) - lambda2)) + eL3*(E3 - lambda3)/((lambda1 - lambda3)*(lambda2 - lambda3)*((*KA) - lambda3)) + eKa*(E3 - (*KA))/((-(*KA) + lambda1)*(-(*KA) + lambda3)*(-(*KA) + lambda2)));
}

static inline void threeCmtKaSSb2(double *A, double *tau, double *b2, 
				  double *KA, double *k20,
				  double *k23, double *k32,
				  double *k24, double *k42) {
  double E2 = (*k20)+(*k23)+(*k24);
  double E3 = (*k32);
  double E4 = (*k42);

  double a = E2+E3+E4;
  double b = E2*E3+E4*(E2+E3)-(*k23)*(*k32)-(*k24)*(*k42);
  double c = E2*E3*E4-E4*(*k23)*(*k32)-E3*(*k24)*(*k42);

  double a2 = a*a;
  double m = 0.333333333333333*(3.0*b - a2);
  double n = 0.03703703703703703*(2.0*a2*a - 9.0*a*b + 27.0*c);
  double Q = 0.25*(n*n) + 0.03703703703703703*(m*m*m);

  double alpha = sqrt(-Q);
  double beta = -0.5*n;
  double gamma = sqrt(beta*beta+alpha*alpha);
  double theta = atan2(alpha,beta);
  double theta3 = 0.333333333333333*theta;
  double ctheta3 = cos(theta3);
  double stheta3 = 1.7320508075688771932*sin(theta3);
  double gamma3 = R_pow(gamma,0.333333333333333);
  
  double lambda1 = 0.333333333333333*a + gamma3*(ctheta3 + stheta3);
  double lambda2 = 0.333333333333333*a + gamma3*(ctheta3 -stheta3);
  double lambda3 = 0.333333333333333*a -(2.0*gamma3*ctheta3);

  double eL1 = 1.0/(1.0-exp(-(*tau)*lambda1));
  double eL2 = 1.0/(1.0-exp(-(*tau)*lambda2));
  double eL3 = 1.0/(1.0-exp(-(*tau)*lambda3));
  A1=0.0;
  A2=(*b2)*(eL1*(E3 - lambda1)*(E4 - lambda1)/((-lambda1 + lambda3)*(-lambda1 + lambda2)) + eL2*(E3 - lambda2)*(E4 - lambda2)/((lambda1 - lambda2)*(-lambda2 + lambda3)) + eL3*(E3 - lambda3)*(E4 - lambda3)/((lambda1 - lambda3)*(lambda2 - lambda3)));
  A3=eL2*(-(*b2)*E4*(*k23) + (*b2)*(*k23)*lambda2)/((lambda1 - lambda2)*(lambda2 - lambda3)) + eL1*((*b2)*E4*(*k23) - (*b2)*(*k23)*lambda1)/((lambda1 - lambda3)*(lambda1 - lambda2)) + eL3*(-(*b2)*E4*(*k23) + (*b2)*(*k23)*lambda3)/((lambda1 - lambda3)*(-lambda2 + lambda3));
  A4=eL2*(-(*b2)*E3*(*k24) + (*b2)*(*k24)*lambda2)/((lambda1 - lambda2)*(lambda2 - lambda3)) + eL1*((*b2)*E3*(*k24) - (*b2)*(*k24)*lambda1)/((lambda1 - lambda3)*(lambda1 - lambda2)) + eL3*(-(*b2)*E3*(*k24) + (*b2)*(*k24)*lambda3)/((lambda1 - lambda3)*(-lambda2 + lambda3));
}

static inline void threeCmtKa(double *A, double *Alast, double *t,
			      double *b1, double *b2,
			      double *KA, double *k20,
			      double *k23, double *k32,
			      double *k24, double *k42) {
  double E2 = (*k20)+(*k23)+(*k24);
  double E3 = (*k32);
  double E4 = (*k42);

  double a = E2+E3+E4;
  double b = E2*E3+E4*(E2+E3)-(*k23)*(*k32)-(*k24)*(*k42);
  double c = E2*E3*E4-E4*(*k23)*(*k32)-E3*(*k24)*(*k42);

  double a2 = a*a;
  double m = 0.333333333333333*(3.0*b - a2);
  double n = 0.03703703703703703*(2.0*a2*a - 9.0*a*b + 27.0*c);
  double Q = 0.25*(n*n) + 0.03703703703703703*(m*m*m);

  double alpha = sqrt(-Q);
  double beta = -0.5*n;
  double gamma = sqrt(beta*beta+alpha*alpha);
  double theta = atan2(alpha,beta);
  double theta3 = 0.333333333333333*theta;
  double ctheta3 = cos(theta3);
  double stheta3 = 1.7320508075688771932*sin(theta3);
  double gamma3 = R_pow(gamma,0.333333333333333);
  
  double lambda1 = 0.333333333333333*a + gamma3*(ctheta3 + stheta3);
  double lambda2 = 0.333333333333333*a + gamma3*(ctheta3 -stheta3);
  double lambda3 = 0.333333333333333*a -(2.0*gamma3*ctheta3);

  double B = A3last*(*k32)+A4last*(*k42);
  double C = E4*A3last*(*k32)+E3*A4last*(*k42);
  double I = A2last*(*k23)*E4-A3last*(*k24)*(*k42)+A4last*(*k23)*(*k42);
  double J = A2last*(*k24)*E3+A3last*(*k24)*(*k32)-A4last*(*k23)*(*k32);

  double eL1 = exp(-(*t)*lambda1);
  double eL2 = exp(-(*t)*lambda2);
  double eL3 = exp(-(*t)*lambda3);
  double eKA = exp(-(*t)*(*KA));

  double l12 = (lambda1-lambda2);
  double l13 = (lambda1-lambda3);
  double l21 = (lambda2-lambda1);
  double l23 = (lambda2-lambda3);
  double l31 = (lambda3-lambda1);
  double l32 = (lambda3-lambda2);

  double e2l1 = (E2-lambda1);
  double e2l2 = (E2-lambda2);
  double e3l1 = (E3-lambda1);
  double e3l2 = (E3-lambda2);
  double e3l3 = (E3-lambda3);
  double e4l1 = (E4-lambda1);
  double e4l2 = (E4-lambda2);
  double e4l3 = (E4-lambda3);
  
  double A2term1 = A2last*(eL1*e3l1*e4l1/(l21*l31)+eL2*e3l2*e4l2/(l12*l32)+eL3*e3l3*e4l3/(l13*l23));
  
  double A2term2 = eL1*(C-B*lambda1)/(l12*l13)+eL2*(B*lambda2-C)/(l12*l23)+eL3*(B*lambda3-C)/(l13*l32);
  
  double A2term3 = A1last*(*KA)*(eL1*e3l1*e4l1/(l21*l31*((*KA)-lambda1))+eL2*e3l2*e4l2/(l12*l32*((*KA)-lambda2))+eL3*e3l3*e4l3/(l13*l23*((*KA)-lambda3))+eKA*(E3-(*KA))*(E4-(*KA))/((lambda1-(*KA))*(lambda2-(*KA))*(lambda3-(*KA))));
  
  A2 = A2term1+A2term2+A2term3 + (*b2);

  double A3term1 = A3last*(eL1*e2l1*e4l1/(l21*l31)+eL2*e2l2*e4l2/(l12*l32)+eL3*(E2-lambda3)*e4l3/(l13*l23));
  
  double A3term2 = eL1*(I-A2last*(*k23)*lambda1)/(l12*l13)+eL2*(A2last*(*k23)*lambda2-I)/(l12*l23)+eL3*(A2last*(*k23)*lambda3-I)/(l13*l32);
  
  double A3term3 = A1last*(*KA)*(*k23)*(eL1*e4l1/(l21*l31*((*KA)-lambda1))+eL2*e4l2/(l12*l32*((*KA)-lambda2))+eL3*e4l3/(l13*l23*((*KA)-lambda3))+eKA*(E4-(*KA))/((lambda1-(*KA))*(lambda2-(*KA))*(lambda3-(*KA))));
  
  A3 = A3term1+A3term2+A3term3;

  double A4term1 = A4last*(eL1*e2l1*e3l1/(l21*l31)+eL2*e2l2*e3l2/(l12*l32)+eL3*(E2-lambda3)*e3l3/(l13*l23));
  
  double A4term2 = eL1*(J-A2last*(*k24)*lambda1)/(l12*l13)+eL2*(A2last*(*k24)*lambda2-J)/(l12*l23)+eL3*(A2last*(*k24)*lambda3-J)/(l13*l32);
  
  double A4term3 = A1last*(*KA)*(*k24)*(eL1*e3l1/(l21*l31*((*KA)-lambda1))+eL2*e3l2/(l12*l32*((*KA)-lambda2))+eL3*e3l3/(l13*l23*((*KA)-lambda3))+eKA*(E3-(*KA))/((lambda1-(*KA))*(lambda2-(*KA))*(lambda3-(*KA))));
  A4 = A4term1+A4term2+A4term3;

  A1 = A1last*eKA + (*b1);
}

////////////////////////////////////////////////////////////////////////////////
// 1-3 compartment bolus during infusion
////////////////////////////////////////////////////////////////////////////////
static inline void oneCmtRateSSr1(double *A, double *r1, double *k10) {
  A1 = (*r1)/(*k10);
}

static inline void oneCmtRateSS(double *A, double *tinf, double *tau, double *r1, double *k10) {
  double eiK = exp(-(*k10)*(*tinf));
  double eK = exp(-(*k10)*((*tau)-(*tinf)))/(1.0-exp(-(*k10)*(*tau)));
  A1=(*r1)*(1-eiK)*eK/((*k10));
}

static inline void oneCmtRate(double *A, double *Alast, 
			      double *t,
			      double *b1, double *r1,
			      double *k10) {
  double eT = exp(-(*k10)*(*t));
  A1 = (*r1)/(*k10)*(1-eT)+A1last*eT + (*b1);
}

static inline void twoCmtRateSSr1(double *A, double *r1,
				  double *k10, double *k12, double *k21) {
  double E1 = (*k10)+(*k12);
  double E2 = (*k21);
  double s = E1+E2;
  double sqr = sqrt(s*s-4*(E1*E2-(*k12)*(*k21)));
  double lambda1 = 0.5*(s+sqr);
  double lambda2 = 0.5*(s-sqr);
  double l12 = 1.0/(lambda1*lambda2);
  A1=(*r1)*E2*l12;
  A2=(*r1)*(*k12)*l12;
}

static inline void twoCmtRateSS(double *A, double *tinf, double *tau, double *r1, 
				double *k10, double *k12, double *k21) {
  double E1 = (*k10)+(*k12);
  double E2 = (*k21);

  double s = E1+E2;
  double sqr = sqrt(s*s-4*(E1*E2-(*k12)*(*k21)));
  double lambda1 = 0.5*(s+sqr);
  double lambda2 = 0.5*(s-sqr);

  double eTi1 = exp(-(*tinf)*lambda1);
  double eTi2 = exp(-(*tinf)*lambda2);
  double eT1 =exp(-lambda1*((*tau)-(*tinf)))/(1.0-exp(-(*tau)*lambda1));
  double eT2 =exp(-lambda2*((*tau)-(*tinf)))/(1.0-exp(-(*tau)*lambda2));
  A1=(eT1*(E2*((eTi1*(*r1) - eTi2*(*r1))/(-lambda1 + lambda2) + (*r1)*E2*(1.0/(lambda1*lambda2) + eTi1/((lambda1 - lambda2)*lambda1) - eTi2/((lambda1 - lambda2)*lambda2))) - lambda1*((eTi1*(*r1) - eTi2*(*r1))/(-lambda1 + lambda2) + (*r1)*E2*(1.0/(lambda1*lambda2) + eTi1/((lambda1 - lambda2)*lambda1) - eTi2/((lambda1 - lambda2)*lambda2))) + (*r1)*(*k12)*(*k21)*(1.0/(lambda1*lambda2) + eTi1/((lambda1 - lambda2)*lambda1) - eTi2/((lambda1 - lambda2)*lambda2))) - eT2*(E2*((eTi1*(*r1) - eTi2*(*r1))/(-lambda1 + lambda2) + (*r1)*E2*(1.0/(lambda1*lambda2) + eTi1/((lambda1 - lambda2)*lambda1) - eTi2/((lambda1 - lambda2)*lambda2))) - lambda2*((eTi1*(*r1) - eTi2*(*r1))/(-lambda1 + lambda2) + (*r1)*E2*(1.0/(lambda1*lambda2) + eTi1/((lambda1 - lambda2)*lambda1) - eTi2/((lambda1 - lambda2)*lambda2))) + (*r1)*(*k12)*(*k21)*(1.0/(lambda1*lambda2) + eTi1/((lambda1 - lambda2)*lambda1) - eTi2/((lambda1 - lambda2)*lambda2))))/(-lambda1 + lambda2);
  A2=(eT1*((*k12)*((eTi1*(*r1) - eTi2*(*r1))/(-lambda1 + lambda2) + (*r1)*E2*(1.0/(lambda1*lambda2) + eTi1/((lambda1 - lambda2)*lambda1) - eTi2/((lambda1 - lambda2)*lambda2))) + (*r1)*E1*(*k12)*(1.0/(lambda1*lambda2) + eTi1/((lambda1 - lambda2)*lambda1) - eTi2/((lambda1 - lambda2)*lambda2)) - (*r1)*(*k12)*lambda1*(1.0/(lambda1*lambda2) + eTi1/((lambda1 - lambda2)*lambda1) - eTi2/((lambda1 - lambda2)*lambda2))) - eT2*((*k12)*((eTi1*(*r1) - eTi2*(*r1))/(-lambda1 + lambda2) + (*r1)*E2*(1.0/(lambda1*lambda2) + eTi1/((lambda1 - lambda2)*lambda1) - eTi2/((lambda1 - lambda2)*lambda2))) + (*r1)*E1*(*k12)*(1.0/(lambda1*lambda2) + eTi1/((lambda1 - lambda2)*lambda1) - eTi2/((lambda1 - lambda2)*lambda2)) - (*r1)*(*k12)*lambda2*(1.0/(lambda1*lambda2) + eTi1/((lambda1 - lambda2)*lambda1) - eTi2/((lambda1 - lambda2)*lambda2))))/(-lambda1 + lambda2);
}

static inline void twoCmtRate(double *A, double *Alast, double *t,
			      double *b1, double *r1,
			      double *k10, double *k12, double *k21) {
  double E1 = (*k10)+(*k12);
  double E2 = (*k21);

  double s = E1+E2;
  double sqr = sqrt(s*s-4*(E1*E2-(*k12)*(*k21)));
  double lambda1 = 0.5*(s+sqr);
  double lambda2 = 0.5*(s-sqr);

  double eT1 = exp(-(*t)*lambda1);
  double eT2 = exp(-(*t)*lambda2);

  double l12 = (lambda1-lambda2);
  double l21 = (lambda2-lambda1);

  double c10 = (A1last*E2+(*r1)+A2last*(*k21));
  double c11 = (c10-A1last*lambda1)/l21;
  double c12 = (c10-A1last*lambda2)/l21;
  double A1term1 = c11*eT1 - c12*eT2;
  double A1term2 = (*r1)*E2*(1/(lambda1*lambda2)+eT1/(lambda1*l12)-eT2/(lambda2*l12));
  A1 = A1term1+A1term2 + (*b1);

  double c20 = (A2last*E1+A1last*(*k12));
  double c21 = (c20-A2last*lambda1)/l21;
  double c22 = (c20-A2last*lambda2)/l21;
  double A2term1 = c21*eT1-c22*eT2;
  double A2term2 = (*r1)*(*k12)*(1/(lambda1*lambda2)+eT1/(lambda1*l12)-eT2/(lambda2*(lambda1-lambda2)));
  A2 = A2term1+A2term2;
}

static inline void threeCmtRateSSr1(double *A, double *r1,
				    double *k10, double *k12, double *k21,
				    double *k13, double *k31) {
  double E1 = (*k10)+(*k12)+(*k13);
  double E2 = (*k21);
  double E3 = (*k31);

  double a = E1+E2+E3;
  double b = E1*E2+E3*(E1+E2)-(*k12)*(*k21)-(*k13)*(*k31);
  double c = E1*E2*E3-E3*(*k12)*(*k21)-E2*(*k13)*(*k31);

  double a2 = a*a;
  double m = 0.333333333333333*(3.0*b - a2);
  double n = 0.03703703703703703*(2.0*a2*a - 9.0*a*b + 27.0*c);
  double Q = 0.25*(n*n) + 0.03703703703703703*(m*m*m);

  double alpha = sqrt(-Q);
  double beta = -0.5*n;
  double gamma = sqrt(beta*beta+alpha*alpha);
  double theta = atan2(alpha,beta);
  double theta3 = 0.333333333333333*theta;
  double ctheta3 = cos(theta3);
  double stheta3 = 1.7320508075688771932*sin(theta3);
  double gamma3 = R_pow(gamma,0.333333333333333);
  
  double lambda1 = 0.333333333333333*a + gamma3*(ctheta3 + stheta3);
  double lambda2 = 0.333333333333333*a + gamma3*(ctheta3 -stheta3);
  double lambda3 = 0.333333333333333*a -(2.0*gamma3*ctheta3);
  double l123 = 1.0/(lambda1*lambda2*lambda3);
  A1=(*r1)*E2*E3*l123;
  A2=(*r1)*E3*(*k12)*l123;
  A3=(*r1)*E2*(*k13)*l123;
}

static inline void threeCmtRateSS(double *A,
				  double *tinf, double *tau, double *r1, 
				  double *k10, double *k12, double *k21,
				  double *k13, double *k31){
  double E1 = (*k10)+(*k12)+(*k13);
  double E2 = (*k21);
  double E3 = (*k31);

  double a = E1+E2+E3;
  double b = E1*E2+E3*(E1+E2)-(*k12)*(*k21)-(*k13)*(*k31);
  double c = E1*E2*E3-E3*(*k12)*(*k21)-E2*(*k13)*(*k31);

  double a2 = a*a;
  double m = 0.333333333333333*(3.0*b - a2);
  double n = 0.03703703703703703*(2.0*a2*a - 9.0*a*b + 27.0*c);
  double Q = 0.25*(n*n) + 0.03703703703703703*(m*m*m);

  double alpha = sqrt(-Q);
  double beta = -0.5*n;
  double gamma = sqrt(beta*beta+alpha*alpha);
  double theta = atan2(alpha,beta);
  double theta3 = 0.333333333333333*theta;
  double ctheta3 = cos(theta3);
  double stheta3 = 1.7320508075688771932*sin(theta3);
  double gamma3 = R_pow(gamma,0.333333333333333);
  
  double lambda1 = 0.333333333333333*a + gamma3*(ctheta3 + stheta3);
  double lambda2 = 0.333333333333333*a + gamma3*(ctheta3 -stheta3);
  double lambda3 = 0.333333333333333*a -(2.0*gamma3*ctheta3);
  double eTi1 = exp(-(*tinf)*lambda1);
  double eTi2 = exp(-(*tinf)*lambda2);
  double eTi3 = exp(-(*tinf)*lambda3);
  double eT1 = exp(-lambda1*((*tau)-(*tinf)))/(1.0-exp(-(*tau)*lambda1));
  double eT2 = exp(-lambda2*((*tau)-(*tinf)))/(1.0-exp(-(*tau)*lambda2));
  double eT3 = exp(-lambda3*((*tau)-(*tinf)))/(1.0-exp(-(*tau)*lambda3));
  A1=(*r1)*(eT1*(E2 - lambda1)*(E3 - lambda1)/((-lambda1 + lambda3)*(-lambda1 + lambda2)) + eT2*(E3 - lambda2)*(E2 - lambda2)/((lambda1 - lambda2)*(-lambda2 + lambda3)) + eT3*(E2 - lambda3)*(E3 - lambda3)/((lambda1 - lambda3)*(lambda2 - lambda3)))*(E2*E3/(lambda1*lambda2*lambda3) - eTi1*(E2 - lambda1)*(E3 - lambda1)/((-lambda1 + lambda3)*(-lambda1 + lambda2)*lambda1) - eTi2*(E3 - lambda2)*(E2 - lambda2)/((lambda1 - lambda2)*(-lambda2 + lambda3)*lambda2) - eTi3*(E2 - lambda3)*(E3 - lambda3)/((lambda1 - lambda3)*(lambda2 - lambda3)*lambda3)) + eT2*(lambda2*((*r1)*(*k12)*(*k21)*(E3/(lambda1*lambda2*lambda3) - eTi1*(E3 - lambda1)/((-lambda1 + lambda3)*(-lambda1 + lambda2)*lambda1) - eTi2*(E3 - lambda2)/((lambda1 - lambda2)*(-lambda2 + lambda3)*lambda2) - eTi3*(E3 - lambda3)/((lambda1 - lambda3)*(lambda2 - lambda3)*lambda3)) + (*r1)*(*k13)*(*k31)*(E2/(lambda1*lambda2*lambda3) - eTi1*(E2 - lambda1)/((-lambda1 + lambda3)*(-lambda1 + lambda2)*lambda1) - eTi2*(E2 - lambda2)/((lambda1 - lambda2)*(-lambda2 + lambda3)*lambda2) - eTi3*(E2 - lambda3)/((lambda1 - lambda3)*(lambda2 - lambda3)*lambda3))) - ((*r1)*E2*(*k13)*(*k31)*(E2/(lambda1*lambda2*lambda3) - eTi1*(E2 - lambda1)/((-lambda1 + lambda3)*(-lambda1 + lambda2)*lambda1) - eTi2*(E2 - lambda2)/((lambda1 - lambda2)*(-lambda2 + lambda3)*lambda2) - eTi3*(E2 - lambda3)/((lambda1 - lambda3)*(lambda2 - lambda3)*lambda3)) + (*r1)*E3*(*k12)*(*k21)*(E3/(lambda1*lambda2*lambda3) - eTi1*(E3 - lambda1)/((-lambda1 + lambda3)*(-lambda1 + lambda2)*lambda1) - eTi2*(E3 - lambda2)/((lambda1 - lambda2)*(-lambda2 + lambda3)*lambda2) - eTi3*(E3 - lambda3)/((lambda1 - lambda3)*(lambda2 - lambda3)*lambda3))))/((lambda1 - lambda2)*(lambda2 - lambda3)) + eT1*(-lambda1*((*r1)*(*k12)*(*k21)*(E3/(lambda1*lambda2*lambda3) - eTi1*(E3 - lambda1)/((-lambda1 + lambda3)*(-lambda1 + lambda2)*lambda1) - eTi2*(E3 - lambda2)/((lambda1 - lambda2)*(-lambda2 + lambda3)*lambda2) - eTi3*(E3 - lambda3)/((lambda1 - lambda3)*(lambda2 - lambda3)*lambda3)) + (*r1)*(*k13)*(*k31)*(E2/(lambda1*lambda2*lambda3) - eTi1*(E2 - lambda1)/((-lambda1 + lambda3)*(-lambda1 + lambda2)*lambda1) - eTi2*(E2 - lambda2)/((lambda1 - lambda2)*(-lambda2 + lambda3)*lambda2) - eTi3*(E2 - lambda3)/((lambda1 - lambda3)*(lambda2 - lambda3)*lambda3))) + (*r1)*E2*(*k13)*(*k31)*(E2/(lambda1*lambda2*lambda3) - eTi1*(E2 - lambda1)/((-lambda1 + lambda3)*(-lambda1 + lambda2)*lambda1) - eTi2*(E2 - lambda2)/((lambda1 - lambda2)*(-lambda2 + lambda3)*lambda2) - eTi3*(E2 - lambda3)/((lambda1 - lambda3)*(lambda2 - lambda3)*lambda3)) + (*r1)*E3*(*k12)*(*k21)*(E3/(lambda1*lambda2*lambda3) - eTi1*(E3 - lambda1)/((-lambda1 + lambda3)*(-lambda1 + lambda2)*lambda1) - eTi2*(E3 - lambda2)/((lambda1 - lambda2)*(-lambda2 + lambda3)*lambda2) - eTi3*(E3 - lambda3)/((lambda1 - lambda3)*(lambda2 - lambda3)*lambda3)))/((lambda1 - lambda3)*(lambda1 - lambda2)) + eT3*(lambda3*((*r1)*(*k12)*(*k21)*(E3/(lambda1*lambda2*lambda3) - eTi1*(E3 - lambda1)/((-lambda1 + lambda3)*(-lambda1 + lambda2)*lambda1) - eTi2*(E3 - lambda2)/((lambda1 - lambda2)*(-lambda2 + lambda3)*lambda2) - eTi3*(E3 - lambda3)/((lambda1 - lambda3)*(lambda2 - lambda3)*lambda3)) + (*r1)*(*k13)*(*k31)*(E2/(lambda1*lambda2*lambda3) - eTi1*(E2 - lambda1)/((-lambda1 + lambda3)*(-lambda1 + lambda2)*lambda1) - eTi2*(E2 - lambda2)/((lambda1 - lambda2)*(-lambda2 + lambda3)*lambda2) - eTi3*(E2 - lambda3)/((lambda1 - lambda3)*(lambda2 - lambda3)*lambda3))) - ((*r1)*E2*(*k13)*(*k31)*(E2/(lambda1*lambda2*lambda3) - eTi1*(E2 - lambda1)/((-lambda1 + lambda3)*(-lambda1 + lambda2)*lambda1) - eTi2*(E2 - lambda2)/((lambda1 - lambda2)*(-lambda2 + lambda3)*lambda2) - eTi3*(E2 - lambda3)/((lambda1 - lambda3)*(lambda2 - lambda3)*lambda3)) + (*r1)*E3*(*k12)*(*k21)*(E3/(lambda1*lambda2*lambda3) - eTi1*(E3 - lambda1)/((-lambda1 + lambda3)*(-lambda1 + lambda2)*lambda1) - eTi2*(E3 - lambda2)/((lambda1 - lambda2)*(-lambda2 + lambda3)*lambda2) - eTi3*(E3 - lambda3)/((lambda1 - lambda3)*(lambda2 - lambda3)*lambda3))))/((lambda1 - lambda3)*(-lambda2 + lambda3));
  A2=(*r1)*(*k12)*(eT1*(E3 - lambda1)*(E1 - lambda1)/((-lambda1 + lambda3)*(-lambda1 + lambda2)) + eT2*(E1 - lambda2)*(E3 - lambda2)/((lambda1 - lambda2)*(-lambda2 + lambda3)) + eT3*(E1 - lambda3)*(E3 - lambda3)/((lambda1 - lambda3)*(lambda2 - lambda3)))*(E3/(lambda1*lambda2*lambda3) - eTi1*(E3 - lambda1)/((-lambda1 + lambda3)*(-lambda1 + lambda2)*lambda1) - eTi2*(E3 - lambda2)/((lambda1 - lambda2)*(-lambda2 + lambda3)*lambda2) - eTi3*(E3 - lambda3)/((lambda1 - lambda3)*(lambda2 - lambda3)*lambda3)) + eT2*((*r1)*(*k12)*(E2*E3/(lambda1*lambda2*lambda3) - eTi1*(E2 - lambda1)*(E3 - lambda1)/((-lambda1 + lambda3)*(-lambda1 + lambda2)*lambda1) - eTi2*(E3 - lambda2)*(E2 - lambda2)/((lambda1 - lambda2)*(-lambda2 + lambda3)*lambda2) - eTi3*(E2 - lambda3)*(E3 - lambda3)/((lambda1 - lambda3)*(lambda2 - lambda3)*lambda3))*lambda2 - ((*r1)*E3*(*k12)*(E2*E3/(lambda1*lambda2*lambda3) - eTi1*(E2 - lambda1)*(E3 - lambda1)/((-lambda1 + lambda3)*(-lambda1 + lambda2)*lambda1) - eTi2*(E3 - lambda2)*(E2 - lambda2)/((lambda1 - lambda2)*(-lambda2 + lambda3)*lambda2) - eTi3*(E2 - lambda3)*(E3 - lambda3)/((lambda1 - lambda3)*(lambda2 - lambda3)*lambda3)) + (*r1)*(*k13)*(*k12)*(*k31)*(E2/(lambda1*lambda2*lambda3) - eTi1*(E2 - lambda1)/((-lambda1 + lambda3)*(-lambda1 + lambda2)*lambda1) - eTi2*(E2 - lambda2)/((lambda1 - lambda2)*(-lambda2 + lambda3)*lambda2) - eTi3*(E2 - lambda3)/((lambda1 - lambda3)*(lambda2 - lambda3)*lambda3)) - (*r1)*(*k13)*(*k12)*(*k31)*(E3/(lambda1*lambda2*lambda3) - eTi1*(E3 - lambda1)/((-lambda1 + lambda3)*(-lambda1 + lambda2)*lambda1) - eTi2*(E3 - lambda2)/((lambda1 - lambda2)*(-lambda2 + lambda3)*lambda2) - eTi3*(E3 - lambda3)/((lambda1 - lambda3)*(lambda2 - lambda3)*lambda3))))/((lambda1 - lambda2)*(lambda2 - lambda3)) + eT1*((*r1)*E3*(*k12)*(E2*E3/(lambda1*lambda2*lambda3) - eTi1*(E2 - lambda1)*(E3 - lambda1)/((-lambda1 + lambda3)*(-lambda1 + lambda2)*lambda1) - eTi2*(E3 - lambda2)*(E2 - lambda2)/((lambda1 - lambda2)*(-lambda2 + lambda3)*lambda2) - eTi3*(E2 - lambda3)*(E3 - lambda3)/((lambda1 - lambda3)*(lambda2 - lambda3)*lambda3)) - (*r1)*(*k12)*(E2*E3/(lambda1*lambda2*lambda3) - eTi1*(E2 - lambda1)*(E3 - lambda1)/((-lambda1 + lambda3)*(-lambda1 + lambda2)*lambda1) - eTi2*(E3 - lambda2)*(E2 - lambda2)/((lambda1 - lambda2)*(-lambda2 + lambda3)*lambda2) - eTi3*(E2 - lambda3)*(E3 - lambda3)/((lambda1 - lambda3)*(lambda2 - lambda3)*lambda3))*lambda1 + (*r1)*(*k13)*(*k12)*(*k31)*(E2/(lambda1*lambda2*lambda3) - eTi1*(E2 - lambda1)/((-lambda1 + lambda3)*(-lambda1 + lambda2)*lambda1) - eTi2*(E2 - lambda2)/((lambda1 - lambda2)*(-lambda2 + lambda3)*lambda2) - eTi3*(E2 - lambda3)/((lambda1 - lambda3)*(lambda2 - lambda3)*lambda3)) - (*r1)*(*k13)*(*k12)*(*k31)*(E3/(lambda1*lambda2*lambda3) - eTi1*(E3 - lambda1)/((-lambda1 + lambda3)*(-lambda1 + lambda2)*lambda1) - eTi2*(E3 - lambda2)/((lambda1 - lambda2)*(-lambda2 + lambda3)*lambda2) - eTi3*(E3 - lambda3)/((lambda1 - lambda3)*(lambda2 - lambda3)*lambda3)))/((lambda1 - lambda3)*(lambda1 - lambda2)) + eT3*((*r1)*(*k12)*(E2*E3/(lambda1*lambda2*lambda3) - eTi1*(E2 - lambda1)*(E3 - lambda1)/((-lambda1 + lambda3)*(-lambda1 + lambda2)*lambda1) - eTi2*(E3 - lambda2)*(E2 - lambda2)/((lambda1 - lambda2)*(-lambda2 + lambda3)*lambda2) - eTi3*(E2 - lambda3)*(E3 - lambda3)/((lambda1 - lambda3)*(lambda2 - lambda3)*lambda3))*lambda3 - ((*r1)*E3*(*k12)*(E2*E3/(lambda1*lambda2*lambda3) - eTi1*(E2 - lambda1)*(E3 - lambda1)/((-lambda1 + lambda3)*(-lambda1 + lambda2)*lambda1) - eTi2*(E3 - lambda2)*(E2 - lambda2)/((lambda1 - lambda2)*(-lambda2 + lambda3)*lambda2) - eTi3*(E2 - lambda3)*(E3 - lambda3)/((lambda1 - lambda3)*(lambda2 - lambda3)*lambda3)) + (*r1)*(*k13)*(*k12)*(*k31)*(E2/(lambda1*lambda2*lambda3) - eTi1*(E2 - lambda1)/((-lambda1 + lambda3)*(-lambda1 + lambda2)*lambda1) - eTi2*(E2 - lambda2)/((lambda1 - lambda2)*(-lambda2 + lambda3)*lambda2) - eTi3*(E2 - lambda3)/((lambda1 - lambda3)*(lambda2 - lambda3)*lambda3)) - (*r1)*(*k13)*(*k12)*(*k31)*(E3/(lambda1*lambda2*lambda3) - eTi1*(E3 - lambda1)/((-lambda1 + lambda3)*(-lambda1 + lambda2)*lambda1) - eTi2*(E3 - lambda2)/((lambda1 - lambda2)*(-lambda2 + lambda3)*lambda2) - eTi3*(E3 - lambda3)/((lambda1 - lambda3)*(lambda2 - lambda3)*lambda3))))/((lambda1 - lambda3)*(-lambda2 + lambda3));
  A3=(*r1)*(*k13)*(E2/(lambda1*lambda2*lambda3) - eTi1*(E2 - lambda1)/((-lambda1 + lambda3)*(-lambda1 + lambda2)*lambda1) - eTi2*(E2 - lambda2)/((lambda1 - lambda2)*(-lambda2 + lambda3)*lambda2) - eTi3*(E2 - lambda3)/((lambda1 - lambda3)*(lambda2 - lambda3)*lambda3))*(eT1*(E2 - lambda1)*(E1 - lambda1)/((-lambda1 + lambda3)*(-lambda1 + lambda2)) + eT2*(E1 - lambda2)*(E2 - lambda2)/((lambda1 - lambda2)*(-lambda2 + lambda3)) + eT3*(E1 - lambda3)*(E2 - lambda3)/((lambda1 - lambda3)*(lambda2 - lambda3))) + eT2*((*r1)*(*k13)*(E2*E3/(lambda1*lambda2*lambda3) - eTi1*(E2 - lambda1)*(E3 - lambda1)/((-lambda1 + lambda3)*(-lambda1 + lambda2)*lambda1) - eTi2*(E3 - lambda2)*(E2 - lambda2)/((lambda1 - lambda2)*(-lambda2 + lambda3)*lambda2) - eTi3*(E2 - lambda3)*(E3 - lambda3)/((lambda1 - lambda3)*(lambda2 - lambda3)*lambda3))*lambda2 - ((*r1)*E2*(*k13)*(E2*E3/(lambda1*lambda2*lambda3) - eTi1*(E2 - lambda1)*(E3 - lambda1)/((-lambda1 + lambda3)*(-lambda1 + lambda2)*lambda1) - eTi2*(E3 - lambda2)*(E2 - lambda2)/((lambda1 - lambda2)*(-lambda2 + lambda3)*lambda2) - eTi3*(E2 - lambda3)*(E3 - lambda3)/((lambda1 - lambda3)*(lambda2 - lambda3)*lambda3)) - (*r1)*(*k13)*(*k12)*(*k21)*(E2/(lambda1*lambda2*lambda3) - eTi1*(E2 - lambda1)/((-lambda1 + lambda3)*(-lambda1 + lambda2)*lambda1) - eTi2*(E2 - lambda2)/((lambda1 - lambda2)*(-lambda2 + lambda3)*lambda2) - eTi3*(E2 - lambda3)/((lambda1 - lambda3)*(lambda2 - lambda3)*lambda3)) + (*r1)*(*k13)*(*k12)*(*k21)*(E3/(lambda1*lambda2*lambda3) - eTi1*(E3 - lambda1)/((-lambda1 + lambda3)*(-lambda1 + lambda2)*lambda1) - eTi2*(E3 - lambda2)/((lambda1 - lambda2)*(-lambda2 + lambda3)*lambda2) - eTi3*(E3 - lambda3)/((lambda1 - lambda3)*(lambda2 - lambda3)*lambda3))))/((lambda1 - lambda2)*(lambda2 - lambda3)) + eT1*((*r1)*E2*(*k13)*(E2*E3/(lambda1*lambda2*lambda3) - eTi1*(E2 - lambda1)*(E3 - lambda1)/((-lambda1 + lambda3)*(-lambda1 + lambda2)*lambda1) - eTi2*(E3 - lambda2)*(E2 - lambda2)/((lambda1 - lambda2)*(-lambda2 + lambda3)*lambda2) - eTi3*(E2 - lambda3)*(E3 - lambda3)/((lambda1 - lambda3)*(lambda2 - lambda3)*lambda3)) - (*r1)*(*k13)*(E2*E3/(lambda1*lambda2*lambda3) - eTi1*(E2 - lambda1)*(E3 - lambda1)/((-lambda1 + lambda3)*(-lambda1 + lambda2)*lambda1) - eTi2*(E3 - lambda2)*(E2 - lambda2)/((lambda1 - lambda2)*(-lambda2 + lambda3)*lambda2) - eTi3*(E2 - lambda3)*(E3 - lambda3)/((lambda1 - lambda3)*(lambda2 - lambda3)*lambda3))*lambda1 - (*r1)*(*k13)*(*k12)*(*k21)*(E2/(lambda1*lambda2*lambda3) - eTi1*(E2 - lambda1)/((-lambda1 + lambda3)*(-lambda1 + lambda2)*lambda1) - eTi2*(E2 - lambda2)/((lambda1 - lambda2)*(-lambda2 + lambda3)*lambda2) - eTi3*(E2 - lambda3)/((lambda1 - lambda3)*(lambda2 - lambda3)*lambda3)) + (*r1)*(*k13)*(*k12)*(*k21)*(E3/(lambda1*lambda2*lambda3) - eTi1*(E3 - lambda1)/((-lambda1 + lambda3)*(-lambda1 + lambda2)*lambda1) - eTi2*(E3 - lambda2)/((lambda1 - lambda2)*(-lambda2 + lambda3)*lambda2) - eTi3*(E3 - lambda3)/((lambda1 - lambda3)*(lambda2 - lambda3)*lambda3)))/((lambda1 - lambda3)*(lambda1 - lambda2)) + eT3*((*r1)*(*k13)*(E2*E3/(lambda1*lambda2*lambda3) - eTi1*(E2 - lambda1)*(E3 - lambda1)/((-lambda1 + lambda3)*(-lambda1 + lambda2)*lambda1) - eTi2*(E3 - lambda2)*(E2 - lambda2)/((lambda1 - lambda2)*(-lambda2 + lambda3)*lambda2) - eTi3*(E2 - lambda3)*(E3 - lambda3)/((lambda1 - lambda3)*(lambda2 - lambda3)*lambda3))*lambda3 - ((*r1)*E2*(*k13)*(E2*E3/(lambda1*lambda2*lambda3) - eTi1*(E2 - lambda1)*(E3 - lambda1)/((-lambda1 + lambda3)*(-lambda1 + lambda2)*lambda1) - eTi2*(E3 - lambda2)*(E2 - lambda2)/((lambda1 - lambda2)*(-lambda2 + lambda3)*lambda2) - eTi3*(E2 - lambda3)*(E3 - lambda3)/((lambda1 - lambda3)*(lambda2 - lambda3)*lambda3)) - (*r1)*(*k13)*(*k12)*(*k21)*(E2/(lambda1*lambda2*lambda3) - eTi1*(E2 - lambda1)/((-lambda1 + lambda3)*(-lambda1 + lambda2)*lambda1) - eTi2*(E2 - lambda2)/((lambda1 - lambda2)*(-lambda2 + lambda3)*lambda2) - eTi3*(E2 - lambda3)/((lambda1 - lambda3)*(lambda2 - lambda3)*lambda3)) + (*r1)*(*k13)*(*k12)*(*k21)*(E3/(lambda1*lambda2*lambda3) - eTi1*(E3 - lambda1)/((-lambda1 + lambda3)*(-lambda1 + lambda2)*lambda1) - eTi2*(E3 - lambda2)/((lambda1 - lambda2)*(-lambda2 + lambda3)*lambda2) - eTi3*(E3 - lambda3)/((lambda1 - lambda3)*(lambda2 - lambda3)*lambda3))))/((lambda1 - lambda3)*(-lambda2 + lambda3));
}

static inline void threeCmtRate(double *A, double *Alast,
				double *t, double *b1, double *r1,
				double *k10, double *k12, double *k21,
				double *k13, double *k31) {
  double E1 = (*k10)+(*k12)+(*k13);
  double E2 = (*k21);
  double E3 = (*k31);

  double a = E1+E2+E3;
  double b = E1*E2+E3*(E1+E2)-(*k12)*(*k21)-(*k13)*(*k31);
  double c = E1*E2*E3-E3*(*k12)*(*k21)-E2*(*k13)*(*k31);

  double a2 = a*a;
  double m = 0.333333333333333*(3.0*b - a2);
  double n = 0.03703703703703703*(2.0*a2*a - 9.0*a*b + 27.0*c);
  double Q = 0.25*(n*n) + 0.03703703703703703*(m*m*m);

  double alpha = sqrt(-Q);
  double beta = -0.5*n;
  double gamma = sqrt(beta*beta+alpha*alpha);
  double theta = atan2(alpha,beta);
  double theta3 = 0.333333333333333*theta;
  double ctheta3 = cos(theta3);
  double stheta3 = 1.7320508075688771932*sin(theta3);
  double gamma3 = R_pow(gamma,0.333333333333333);
  
  double lambda1 = 0.333333333333333*a + gamma3*(ctheta3 + stheta3);
  double lambda2 = 0.333333333333333*a + gamma3*(ctheta3 -stheta3);
  double lambda3 = 0.333333333333333*a -(2.0*gamma3*ctheta3);

  double B = A2last*(*k21)+A3last*(*k31);
  double C = E3*A2last*(*k21)+E2*A3last*(*k31);
  double I = A1last*(*k12)*E3-A2last*(*k13)*(*k31)+A3last*(*k12)*(*k31);
  double J = A1last*(*k13)*E2+A2last*(*k13)*(*k21)-A3last*(*k12)*(*k21);

  double eL1 = exp(-(*t)*lambda1);
  double eL2 = exp(-(*t)*lambda2);
  double eL3 = exp(-(*t)*lambda3);

  double l12 = (lambda1-lambda2);
  double l13 = (lambda1-lambda3);
  double l21 = (lambda2-lambda1);
  double l23 = (lambda2-lambda3);
  double l31 = (lambda3-lambda1);
  double l32 = (lambda3-lambda2);
  
  double e1l1 = (E1-lambda1);
  double e1l2 = (E1-lambda2);
  double e1l3 = (E1-lambda3);
  double e2l1 = (E2-lambda1);
  double e2l2 = (E2-lambda2);
  double e2l3 = (E2-lambda3);
  double e3l1 = (E3-lambda1);
  double e3l2 = (E3-lambda2);
  double e3l3 = (E3-lambda3);

  double A1term1 = A1last*(eL1*e2l1*e3l1/(l21*l31)+eL2*e2l2*e3l2/(l12*l32)+eL3*e2l3*e3l3/(l13*l23));
  double A1term2 = eL1*(C-B*lambda1)/(l12*l13)+eL2*(B*lambda2-C)/(l12*l23)+eL3*(B*lambda3-C)/(l13*l32);
  double A1term3 = (*r1)*((E2*E3)/(lambda1*lambda2*lambda3)-eL1*e2l1*e3l1/(lambda1*l21*l31)-eL2*e2l2*e3l2/(lambda2*l12*l32)-eL3*e2l3*e3l3/(lambda3*l13*l23));

  A1 = A1term1+A1term2+A1term3 + (*b1);

  double A2term1 = A2last*(eL1*e1l1*e3l1/(l21*l31)+eL2*e1l2*e3l2/(l12*l32)+eL3*e1l3*e3l3/(l13*l23));
  double A2term2 = eL1*(I-A1last*(*k12)*lambda1)/(l12*l13)+eL2*(A1last*(*k12)*lambda2-I)/(l12*l23)+eL3*(A1last*(*k12)*lambda3-I)/(l13*l32);
  double A2term3 = (*r1)*(*k12)*(E3/(lambda1*lambda2*lambda3)-eL1*e3l1/(lambda1*l21*l31)-eL2*e3l2/(lambda2*l12*l32)-eL3*e3l3/(lambda3*l13*l23));

  A2 = A2term1+A2term2+A2term3;

  double A3term1 = A3last*(eL1*e1l1*e2l1/(l21*l31)+eL2*e1l2*e2l2/(l12*l32)+eL3*e1l3*e2l3/(l13*l23));
  double A3term2 = eL1*(J-A1last*(*k13)*lambda1)/(l12*l13)+eL2*(A1last*(*k13)*lambda2-J)/(l12*l23)+eL3*(A1last*(*k13)*lambda3-J)/(l13*l32);
  double A3term3 = (*r1)*(*k13)*(E2/(lambda1*lambda2*lambda3)-eL1*e2l1/(lambda1*l21*l31)-eL2*e2l2/(lambda2*l12*l32)-eL3*e2l3/(lambda3*l13*l23));

  A3 = A3term1+A3term2+A3term3;
}

////////////////////////////////////////////////////////////////////////////////
// 1-3 compartment bolus only
//
static inline void oneCmtBolusSS(double *A, double *tau,
				 double *b1, double *k10) {
  double eT = 1.0/(1.0-exp(-(*k10)*(*tau)));
  A1 = (*b1)*eT;
}

static inline void oneCmtBolus(double *A, double *Alast, 
			       double *t, double *b1, double *k10) {
  A1 = A1last*exp(-(*k10)*(*t)) + (*b1);
}

static inline void twoCmtBolusSS(double *A,
				 double *tau, double *b1, double *k10,
				 double *k12, double *k21) {
  double E2 = (*k21);

  double s = (*k12)+(*k21)+(*k10);
  double sqr = sqrt(s*s-4*(*k21)*(*k10));
  double lambda1 = 0.5*(s+sqr);
  double lambda2 = 0.5*(s-sqr);

  double eL1 = 1.0/(1.0-exp(-(*tau)*lambda1));
  double eL2 = 1.0/(1.0-exp(-(*tau)*lambda2));
  
  A1=(eL1*((*b1)*E2 - (*b1)*lambda1) - eL2*((*b1)*E2 - (*b1)*lambda2))/(-lambda1 + lambda2);
  A2=(eL1*(*b1)*(*k12) - eL2*(*b1)*(*k12))/(-lambda1 + lambda2);
}

static inline void twoCmtBolus(double *A, double *Alast,
			       double *t, double *b1, double *k10,
			       double *k12, double *k21) {
  double E1 = (*k10)+(*k12);
  double E2 = (*k21);

  double s = (*k12)+(*k21)+(*k10);
  double sqr = sqrt(s*s-4*(*k21)*(*k10));
  double lambda1 = 0.5*(s+sqr);
  double lambda2 = 0.5*(s-sqr);

  double eT1= exp(-(*t)*lambda1);
  double eT2= exp(-(*t)*lambda2);

  double A1term = (((A1last*E2+A2last*(*k21))-A1last*lambda1)*eT1-((A1last*E2+A2last*(*k21))-A1last*lambda2)*eT2)/(lambda2-lambda1);
  
  A1 = A1term + (*b1); 

  double A2term = (((A2last*E1+A1last*(*k12))-A2last*lambda1)*eT1-((A2last*E1+A1last*(*k12))-A2last*lambda2)*eT2)/(lambda2-lambda1);
  A2 = A2term;
}

static inline void threeCmtBolusSS(double *A, double *tau, double *b1, double *k10,
				   double *k12, double *k21,
				   double *k13, double *k31){
  double E1 = (*k10)+(*k12)+(*k13);
  double E2 = (*k21);
  double E3 = (*k31);

  double a = E1+E2+E3;
  double b = E1*E2+E3*(E1+E2)-(*k12)*(*k21)-(*k13)*(*k31);
  double c = E1*E2*E3-E3*(*k12)*(*k21)-E2*(*k13)*(*k31);

  double a2 = a*a;
  double m = 0.333333333333333*(3.0*b - a2);
  double n = 0.03703703703703703*(2.0*a2*a - 9.0*a*b + 27.0*c);
  double Q = 0.25*(n*n) + 0.03703703703703703*(m*m*m);

  double alpha = sqrt(-Q);
  double beta = -0.5*n;
  double gamma = sqrt(beta*beta+alpha*alpha);
  double theta = atan2(alpha,beta);

  double theta3 = 0.333333333333333*theta;
  double ctheta3 = cos(theta3);
  double stheta3 = 1.7320508075688771932*sin(theta3);
  double gamma3 = R_pow(gamma,0.333333333333333);
  double lambda1 = 0.333333333333333*a + gamma3*(ctheta3 + stheta3);
  double lambda2 = 0.333333333333333*a + gamma3*(ctheta3 -stheta3);
  double lambda3 = 0.333333333333333*a -(2*gamma3*ctheta3);

  double eL1 = 1.0/(1.0-exp(-(*tau)*lambda1));
  double eL2 = 1.0/(1.0-exp(-(*tau)*lambda2));
  double eL3 = 1.0/(1.0-exp(-(*tau)*lambda3));

  A1=(*b1)*(eL1*(E2 - lambda1)*(E3 - lambda1)/((-lambda1 + lambda3)*(-lambda1 + lambda2)) + eL2*(E3 - lambda2)*(E2 - lambda2)/((lambda1 - lambda2)*(-lambda2 + lambda3)) + eL3*(E2 - lambda3)*(E3 - lambda3)/((lambda1 - lambda3)*(lambda2 - lambda3)));
  A2=eL2*(-(*b1)*E3*(*k12) + (*b1)*(*k12)*lambda2)/((lambda1 - lambda2)*(lambda2 - lambda3)) + eL1*((*b1)*E3*(*k12) - (*b1)*(*k12)*lambda1)/((lambda1 - lambda3)*(lambda1 - lambda2)) + eL3*(-(*b1)*E3*(*k12) + (*b1)*(*k12)*lambda3)/((lambda1 - lambda3)*(-lambda2 + lambda3));
  A3=eL2*(-(*b1)*E2*(*k13) + (*b1)*(*k13)*lambda2)/((lambda1 - lambda2)*(lambda2 - lambda3)) + eL1*((*b1)*E2*(*k13) - (*b1)*(*k13)*lambda1)/((lambda1 - lambda3)*(lambda1 - lambda2)) + eL3*(-(*b1)*E2*(*k13) + (*b1)*(*k13)*lambda3)/((lambda1 - lambda3)*(-lambda2 + lambda3));
}

static inline void threeCmtBolus(double *A, double *Alast,
				 double *t, double *b1, double *k10,
				 double *k12, double *k21,
				 double *k13, double *k31){
  double E1 = (*k10)+(*k12)+(*k13);
  double E2 = (*k21);
  double E3 = (*k31);

  double a = E1+E2+E3;
  double b = E1*E2+E3*(E1+E2)-(*k12)*(*k21)-(*k13)*(*k31);
  double c = E1*E2*E3-E3*(*k12)*(*k21)-E2*(*k13)*(*k31);

  double a2 = a*a;
  double m = 0.333333333333333*(3.0*b - a2);
  double n = 0.03703703703703703*(2.0*a2*a - 9.0*a*b + 27.0*c);
  double Q = 0.25*(n*n) + 0.03703703703703703*(m*m*m);

  double alpha = sqrt(-Q);
  double beta = -0.5*n;
  double gamma = sqrt(beta*beta+alpha*alpha);
  double theta = atan2(alpha,beta);

  double theta3 = 0.333333333333333*theta;
  double ctheta3 = cos(theta3);
  double stheta3 = 1.7320508075688771932*sin(theta3);
  double gamma3 = R_pow(gamma,0.333333333333333);
  double lambda1 = 0.333333333333333*a + gamma3*(ctheta3 + stheta3);
  double lambda2 = 0.333333333333333*a + gamma3*(ctheta3 -stheta3);
  double lambda3 = 0.333333333333333*a -(2*gamma3*ctheta3);

  double B = A2last*(*k21)+A3last*(*k31);
  double C = E3*A2last*(*k21)+E2*A3last*(*k31);
  double I = A1last*(*k12)*E3-A2last*(*k13)*(*k31)+A3last*(*k12)*(*k31);
  double J = A1last*(*k13)*E2+A2last*(*k13)*(*k21)-A3last*(*k12)*(*k21);

  double eL1 = exp(-(*t)*lambda1);
  double eL2 = exp(-(*t)*lambda2);
  double eL3 = exp(-(*t)*lambda3);

  double l12 = (lambda1-lambda2);
  double l13 = (lambda1-lambda3);
  double l21 = (lambda2-lambda1);
  double l23 = (lambda2-lambda3);
  double l31 = (lambda3-lambda1);
  double l32 = (lambda3-lambda2);
  
  double e1l1 = (E1-lambda1);
  double e1l2 = (E1-lambda2);
  double e1l3 = (E1-lambda3);
  double e2l1 = (E2-lambda1);
  double e2l2 = (E2-lambda2);
  double e2l3 = (E2-lambda3);
  double e3l1 = (E3-lambda1);
  double e3l2 = (E3-lambda2);
  double e3l3 = (E3-lambda3);

  double A1term1 = A1last*(eL1*e2l1*e3l1/(l21*l31)+eL2*e2l2*e3l2/(l12*l32)+eL3*e2l3*e3l3/(l13*l23));
  double A1term2 = eL1*(C-B*lambda1)/(l12*l13)+eL2*(B*lambda2-C)/(l12*l23)+eL3*(B*lambda3-C)/(l13*l32);

  A1 = (*b1)+(A1term1+A1term2);

  double A2term1 = A2last*(eL1*e1l1*e3l1/(l21*l31)+eL2*e1l2*e3l2/(l12*l32)+eL3*e1l3*e3l3/(l13*l23));
  double A2term2 = eL1*(I-A1last*(*k12)*lambda1)/(l12*l13)+eL2*(A1last*(*k12)*lambda2-I)/(l12*l23)+eL3*(A1last*(*k12)*lambda3-I)/(l13*l32);

  A2 = A2term1+A2term2;

  double A3term1 = A3last*(eL1*e1l1*e2l1/(l21*l31)+eL2*e1l2*e2l2/(l12*l32)+eL3*e1l3*e2l3/(l13*l23));
  double A3term2 = eL1*(J-A1last*(*k13)*lambda1)/(l12*l13)+eL2*(A1last*(*k13)*lambda2-J)/(l12*l23)+eL3*(A1last*(*k13)*lambda3-J)/(l13*l32);
  A3 = A3term1+A3term2;
}

static inline void ssRateTau(double *A,
			     int ncmt,
			     int oral0,
			     double *tinf,
			     double *tau,
			     double *r1,
			     double *r2,
			     double *ka, 
			     double *kel,  
			     double *k12, double *k21,
			     double *k13, double *k31){
  if (oral0){
    if ((*r1) > 0 ){
      switch (ncmt){
      case 1: {
	oneCmtKaRateSStr1(A, tinf, tau, r1, ka, kel);
	return;
      } break;
      case 2: {
	twoCmtKaRateSStr1(A, tinf, tau, r1, ka, kel, k12, k21);
	return;
      } break;
      case 3: {
	threeCmtKaRateSStr1(A, tinf, tau,
			    r1, ka, kel, k12, k21, k13, k31);
	return;
      } break;
      }
    } else {
      switch (ncmt){
      case 1: {
	oneCmtKaRateSStr2(A, tinf, tau, r2, ka, kel);
	return;
      } break;
      case 2: {
	twoCmtKaRateSStr2(A, tinf, tau, r2, ka, kel, k12, k21);
	return;
      } break;
      case 3: {
	threeCmtKaRateSStr2(A, tinf, tau, r2, ka, kel, k12,  k21, k13, k31);
	return;
      } break;
      }
    }
  } else {
    switch (ncmt){
    case 1: {
      oneCmtRateSS(A, tinf, tau, r1, kel);
      return;
    } break;
    case 2: {
      twoCmtRateSS(A, tinf, tau, r1, kel, k12, k21);
      return;
    } break;
    case 3: {
      threeCmtRateSS(A, tinf, tau, r1, kel, k12,  k21, k13,  k31);
      return;
    } break;
    }
  }
}

static inline void ssTau(double *A,
			 int ncmt,
			 int oral0,
			 double *tau,
			 double *b1,
			 double *b2,
			 double *ka, // ka (for oral doses)
			 double *kel,  //double rx_v,
			 double *k12, double *k21,
			 double *k13, double *k31){
  if (oral0){
    if ((*b1) > 0 ){
      switch (ncmt){
      case 1: {
	oneCmtKaSSb1(A, tau, b1, ka, kel);
	return;
      } break;
      case 2: {
	twoCmtKaSSb1(A, tau, b1, ka, kel, k12, k21);
	return;
      } break;
      case 3: {
	threeCmtKaSSb1(A, tau, b1, ka, kel, k12,  k21, k13, k31);
	return;
      } break;
      }
    } else {
      switch (ncmt){
      case 1: {
	oneCmtKaSSb2(A, tau, b2, ka, kel);
	return;
      } break;
      case 2: {
	twoCmtKaSSb2(A, tau, b2, ka, kel, k12, k21);
	return;
      } break;
      case 3: {
	threeCmtKaSSb2(A, tau, b2, ka, kel, k12,  k21, k13, k31);
	return;
      } break;
      }
    }
  } else {
    switch (ncmt){
    case 1: {
      oneCmtBolusSS(A, tau, b1, kel);
      return;
    } break;
    case 2: {
      twoCmtBolusSS(A, tau, b1, kel, k12, k21);
      return;
    } break;
    case 3: {
      threeCmtBolusSS(A, tau, b1, kel, k12,  k21, k13,  k31);
      return;
    } break;
    }
  }
}

static inline void ssRate(double *A,
			  int ncmt, // Number of compartments
			  int oral0, // Indicator of if this is an oral system)
			  double *r1, // Rate in Compartment #1
			  double *r2, // Rate in Compartment #2
			  double *ka, // ka (for oral doses)
			  double *kel,  //double rx_v,
			  double *k12, double *k21,
			  double *k13, double *k31) {
  if (oral0){
    if ((*r1) > 0){
      switch (ncmt){
      case 1: {
	oneCmtKaRateSSr1(A, r1, ka, kel);
	return;
      } break;
      case 2: {
	twoCmtKaRateSSr1(A, r1, ka, kel, k12, k21);
	return;
      } break;
      case 3: {
	threeCmtKaRateSSr1(A, r1, ka, kel, k12,  k21, k13,  k31);
	return;
      } break;
      }
    } else {
      switch (ncmt){
      case 1: {
	oneCmtKaRateSSr2(A, r2, ka, kel);
	return;
      } break;
      case 2: {
	twoCmtKaRateSSr2(A, r2, ka, kel, k12, k21);
	return;
      } break;
      case 3: {
	threeCmtKaRateSSr2(A, r2, ka, kel, k12,  k21, k13,  k31);
	return;
      } break;
      }
    }
  } else {
    switch (ncmt){
    case 1: {
      oneCmtRateSSr1(A, r1, kel);
      return;
    } break;
    case 2: {
      twoCmtRateSSr1(A, r1, kel, k12, k21);
      return;
    } break;
    case 3: {
      threeCmtRateSSr1(A, r1, kel, k12,  k21, k13,  k31);
      return;
    } break;
    }
  }
}

static inline void doAdvan(double *A,// Amounts
			   double *Alast, // Last amounts
			   double tlast, // Time of last amounts
			   double ct, // Time of the dose
			   int ncmt, // Number of compartments
			   int oral0, // Indicator of if this is an oral system
			   double *b1, // Amount of the dose in compartment #1
			   double *b2, // Amount of the dose in compartment #2
			   double *r1, // Rate in Compartment #1
			   double *r2, // Rate in Compartment #2
			   double *ka, // ka (for oral doses)
			   double *kel,  //double rx_v,
			   double *k12, double *k21,
			   double *k13, double *k31){
  double t = ct - tlast;
  if ((*r1) > DOUBLE_EPS  || (*r2) > DOUBLE_EPS){
    if (oral0){
      switch (ncmt){
      case 1: {
	oneCmtKaRate(A, Alast, &t, b1, b2, r1, r2, ka, kel);
	return;
      } break;
      case 2: {
	twoCmtKaRate(A, Alast, &t, b1, b2, r1, r2,
		     ka,  kel, k12, k21);
	return;
      } break;
      case 3: {
	threeCmtKaRate(A, Alast, &t, b1, b2, r1, r2, ka, kel, k12,  k21, k13,  k31);
	return;
      } break;
      }
    } else {
      switch (ncmt){
      case 1: {
	oneCmtRate(A, Alast, &t, b1, r1, kel);
	return;
      } break;
      case 2: {
	twoCmtRate(A, Alast, &t, b1, r1,
		   kel, k12, k21);
	return;
      } break;
      case 3: {
	threeCmtRate(A, Alast, &t, b1, r1, kel,
		     k12, k21, k13, k31);
	return;
      } break;

      }
    }
  } else {
    // Bolus doses only
    if (oral0){
      switch (ncmt){
      case 1: {
	oneCmtKa(A, Alast, 
		 &t, b1, b2, ka, kel);
	return;
      } break;
      case 2: {
	twoCmtKa(A, Alast, &t, b1, b2, ka,  kel, k12, k21);
	return;
      } break;
      case 3: {
	threeCmtKa(A, Alast, 
		   &t, b1, b2, ka, kel,
		   k12,  k21, k13,  k31);
	return;
      } break;
      }
    } else {
      // Bolus
      switch (ncmt){
      case 1: {
	oneCmtBolus(A, Alast, &t, b1, kel);
	return;
      } break;
      case 2: {
	twoCmtBolus(A, Alast, &t, b1,
		    kel, k12, k21);
	return;
      } break;
      case 3: {
	threeCmtBolus(A, Alast, &t, b1, kel, k12, k21, k13, k31);
      } break;
      }
    }
  }
}

extern int syncIdx(rx_solving_options_ind *ind);

extern void sortIfNeeded(rx_solve *rx, rx_solving_options_ind *ind, unsigned int id,
			 int *linCmt,
			 double *d_tlag, double *d_tlag2,
			 double *d_F, double *d_F2,
			 double *d_rate1, double *d_dur1,
			 double *d_rate2, double *d_dur2){
  int sort = 0;
  //d_tlag, double d_tlag2,
  /* double d_F, double d_F2, */
  if (ind->lag != *d_tlag){
    ind->lag = *d_tlag;
    sort =1;
  }
  if (ind->lag2 != *d_tlag2){
    ind->lag2 = *d_tlag2;
    sort = 1;
  }
  if (ind->f != *d_F){
    ind->f = *d_F;
    sort = 1;
  }
  if (ind->f2 != *d_F2){
    ind->f2 = *d_F2;
    sort = 1;
  }
  if (ind->rate != *d_rate1) {
    ind->rate = *d_rate1;
    sort = 1;
  }
  if (ind->rate2 != *d_rate2){
    ind->rate2 = *d_rate2;
    sort = 1;
  }
  if (ind->dur != *d_dur1){
    ind->dur = *d_dur1;
    sort = 1;
  }
  if (ind->dur2 != *d_dur2){
    ind->dur2 = *d_dur2;
    sort = 1;
  }
  ind->linCmt = *linCmt;
  if (sort){
    rx->needSort = 1;
    if (rx->nMtime) calcMtime(id, ind->mtime);
    if (rx->needSort) doSort(ind);
  }
}

static inline int parTrans(int *trans, 
			   double *p1, double *v1,
			   double *p2, double *p3,
			   double *p4, double *p5,
			   unsigned int *ncmt, double *rx_k, double *rx_v, double *rx_k12,
			   double *rx_k21, double *rx_k13, double *rx_k31){
  double btemp, ctemp, dtemp;
  if ((*p5) > 0.){
    (*ncmt) = 3;
    switch (*trans){
    case 1: // cl v q vp
      (*rx_k) = (*p1)/(*v1); // k = CL/V
      (*rx_v) = (*v1);
      (*rx_k12) = (*p2)/(*v1); // k12 = Q/V
      (*rx_k21) = (*p2)/(*p3); // k21 = Q/Vp
      (*rx_k13) = (*p4)/(*v1); // k31 = Q2/V
      (*rx_k31) = (*p4)/(*p5); // k31 = Q2/Vp2
      break;
    case 2: // k=(*p1) v=(*v1) k12=(*p2) k21=(*p3) k13=(*p4) k31=(*p5)
      (*rx_k) = (*p1);
      (*rx_v) = (*v1);
      (*rx_k12) = (*p2);
      (*rx_k21) = (*p3);
      (*rx_k13) = (*p4);
      (*rx_k31) = (*p5);
      break;
    case 11:
#undef beta
#define A (1/(*v1))
#define B (*p3)
#define C (*p5)
#define alpha (*p1)
#define beta (*p2)
#define gamma (*p4)
      (*ncmt)=3;
      (*rx_v)=1/(A+B+C);
      btemp = -(alpha*C + alpha*B + gamma*A + gamma*B + beta*A + beta*C)*(*rx_v);
      ctemp = (alpha*beta*C + alpha*gamma*B + beta*gamma*A)*(*rx_v);
      dtemp = sqrt(btemp*btemp-4*ctemp);
      (*rx_k21) = 0.5*(-btemp+dtemp);
      (*rx_k31) = 0.5*(-btemp-dtemp);
      (*rx_k)   = alpha*beta*gamma/(*rx_k21)/(*rx_k31);
      (*rx_k12) = ((beta*gamma + alpha*beta + alpha*gamma) -
		(*rx_k21)*(alpha+beta+gamma) - (*rx_k) * (*rx_k31) + (*rx_k21)*(*rx_k21))/((*rx_k31) - (*rx_k21));
      (*rx_k13) = alpha + beta + gamma - ((*rx_k) + (*rx_k12) + (*rx_k21) + (*rx_k31));
      break;
    case 10:
#undef A
#define A (*v1)
      (*ncmt)=3;
      (*rx_v)=1/(A+B+C);
      btemp = -(alpha*C + alpha*B + gamma*A + gamma*B + beta*A + beta*C)*(*rx_v);
      ctemp = (alpha*beta*C + alpha*gamma*B + beta*gamma*A)*(*rx_v);
      dtemp = sqrt(btemp*btemp-4*ctemp);
      (*rx_k21) = 0.5*(-btemp+dtemp);
      (*rx_k31) = 0.5*(-btemp-dtemp);
      (*rx_k)   = alpha*beta*gamma/(*rx_k21)/(*rx_k31);
      (*rx_k12) = ((beta*gamma + alpha*beta + alpha*gamma) -
		(*rx_k21)*(alpha+beta+gamma) - (*rx_k) * (*rx_k31) + (*rx_k21)*(*rx_k21))/((*rx_k31) - (*rx_k21));
      (*rx_k13) = alpha + beta + gamma - ((*rx_k) + (*rx_k12) + (*rx_k21) + (*rx_k31));
#undef A
#undef B
#undef C
#undef alpha
#undef beta
#undef gamma
#define beta Rf_beta
      break;
    default:
      REprintf(_("invalid trans (3 cmt trans %d)\n"), *trans);
      return NA_REAL;
    }
  } else if ((*p3) > 0.){
    (*ncmt) = 2;
    switch (*trans){
    case 1: // cl=(*p1) v=(*v1) q=(*p2) vp=(*p3)
      (*rx_k) = (*p1)/(*v1); // k = CL/V
      (*rx_v) = (*v1);
      (*rx_k12) = (*p2)/(*v1); // k12 = Q/V
      (*rx_k21) = (*p2)/(*p3); // k21 = Q/Vp
      break;
    case 2: // k=(*p1), (*v1)=v k12=(*p2) k21=(*p3)
      (*rx_k) = (*p1);
      (*rx_v) = (*v1);
      (*rx_k12) = (*p2);
      (*rx_k21) = (*p3);
      break;
    case 3: // cl=(*p1) v=(*v1) q=(*p2) vss=(*p3)
      (*rx_k) = (*p1)/(*v1); // k = CL/V
      (*rx_v) = (*v1);
      (*rx_k12) = (*p2)/(*v1); // k12 = Q/V
      (*rx_k21) = (*p2)/((*p3)-(*v1)); // k21 = Q/(Vss-V)
      break;
    case 4: // alpha=(*p1) beta=(*p2) k21=(*p3)
      (*rx_v) = (*v1);
      (*rx_k21) = (*p3);
      (*rx_k) = (*p1)*(*p2)/(*rx_k21); // (*p1) = alpha (*p2) = beta
      (*rx_k12) = (*p1) + (*p2) - (*rx_k21) - (*rx_k);
      break;
    case 5: // alpha=(*p1) beta=(*p2) aob=(*p3)
      (*rx_v)=(*v1);
      (*rx_k21) = ((*p3)*(*p2)+(*p1))/((*p3)+1.0);
      (*rx_k) = ((*p1)*(*p2))/(*rx_k21);
      (*rx_k12) = (*p1) + (*p2) - (*rx_k21) - (*rx_k);
      break;
    case 11: // A2 V, alpha=(*p1), beta=(*p2), k21
#undef beta
#define A (1/(*v1))
#define B (*p3)
#define alpha (*p1)
#define beta (*p2)
      (*ncmt)=2;
      (*rx_v)   = 1/(A+B);
      (*rx_k21) = (A*beta + B*alpha)*(*rx_v);
      (*rx_k)   = alpha*beta/(*rx_k21);
      (*rx_k12) = alpha+beta-(*rx_k21)-(*rx_k);
      break;
    case 10: // A=(*v1), alpha=(*p1), beta=(*p2), B=(*p3)
      // Convert to A (right now A=(*v1) or A=1/(*v1))
#undef A
#define A (*v1)
      (*ncmt)=2;
      (*rx_v)   = 1/(A + B);
      (*rx_k21) = (A*beta + B*alpha)*(*rx_v);
      (*rx_k)   = alpha*beta/(*rx_k21);
      (*rx_k12) = alpha + beta - (*rx_k21) - (*rx_k);
      /* REprintf("A: %f, B: %f, alpha: %f, beta: %f\n", A, B, alpha, beta); */
      /* REprintf("V: %f, k10: %f, k12: %f, k21: %f\n", rx_v, rx_k, rx_k12, rx_k21); */
#undef A
#undef B
#undef alpha
#undef beta
#define beta Rf_beta
      break;
    default:
      REprintf(_("invalid trans (2 cmt trans %d)\n"), trans);
      return NA_REAL;
    }
  } else if ((*p1) > 0.){
    (*ncmt) = 1;
    switch(*trans){
    case 1: // cl v
      (*rx_k) = (*p1)/(*v1); // k = CL/V
      (*rx_v) = (*v1);
      break;
    case 2: // k V
      (*rx_k) = (*p1);
      (*rx_v) = (*v1);
      break;
    case 11: // alpha V
      (*rx_k) = (*p1);
      (*rx_v) = (*v1);
      break;
    case 10: // alpha A
      (*rx_k) = (*p1);
      (*rx_v) = 1/(*v1);
      break;
    default:
      return 0;
    }
  } else {
    return 0;
  }
  return 1;
}

void linCmtPar1(double *v, double *k, 
		double *vss,
		double *cl,
		double *A,
		double *Af,
		double *alpha,
		double *t12alpha) {
  *vss = *v;
  *cl = (*v)*(*k);
  *A = 1/(*v);
  *alpha = (*k);
  *t12alpha = M_LN2/(*k);
  *Af = (*A)*(*v); // Always 1.
}

void linCmtPar2(double *v, double *k,
		double *k12, double *k21,
		double *vp, double *vss,
		double *cl, double *q,
		double *A, double *B,
		double *Af, double *Bf,
		double *alpha, double *beta,
		double *t12alpha, double *t12beta){
    *vp = (*v)*(*k12)/(*k21);
    *vss = (*v)+(*vp);
    *cl = (*v)*(*k);
    *q  = (*v)*(*k12);
    double a0 = (*k) * (*k21);
    double a1 = -((*k) + (*k12) + (*k21));
    double sq = sqrt(a1*a1-4*a0);
    *alpha = 0.5*(-a1+sq);
    *beta = 0.5*(-a1-sq);
    *A = ((*k21)-(*alpha))/((*beta)-(*alpha))/(*v);
    *B = ((*k21)-(*beta))/((*alpha)-(*beta))/(*v);
    *Af = (*A)*(*v);
    *Bf = (*B)*(*v);
    *t12alpha = M_LN2/(*alpha);
    *t12beta = M_LN2/(*beta);
}

void linCmtPar3(double *v, double *k10,
		double *k12, double *k21, double *k13, double *k31,
		double *vp, double *vp2, double *vss,
		double *cl, double *q, double *q2,
		double *A, double *B, double *C,
		double *Af, double *Bf, double *Cf,
		double *alpha, double *beta, double *gamma,
		double *t12alpha, double *t12beta, double *t12gamma) {
  double a0 = (*k10) * (*k21) * (*k31);
  double a1 = ((*k10) * (*k31)) + ((*k21) * (*k31)) + ((*k21) * (*k13)) + ((*k10) * (*k21)) + ((*k31) * (*k12));
  double a2 = (*k10) + (*k12) + (*k13) + (*k21) + (*k31);
  double p   = a1 - (a2 * a2 / 3.0);
  double qq   = (2.0 * a2 * a2 * a2 / 27.0) - (a1 * a2 / 3.0) + a0;
  double r1  = sqrt(-(p * p * p)/27.0);
  double phi = acos((-qq/2)/r1)/3.0;
  double r2  = 2.0 * exp(log(r1)/3.0);
  *alpha = -(cos(phi) * r2 - a2/3.0);
  *beta = -(cos(phi + 2.0 * M_PI/3.0) * r2 - a2/3.0);
  *gamma = -(cos(phi + 4.0 * M_PI/3.0) * r2 - a2/3.0);
  double a;
  if ((*alpha) < (*beta)) {
    a      = *beta;
    *beta  = *alpha;
    *alpha = a;
  } // now alpha >= beta
  if ((*beta) < (*gamma)) {
    a      = *beta;
    *beta  = *gamma;
    *gamma = a;
  } // now beta >= gamma
  if ((*alpha) < (*beta)) {
    a      = *alpha;
    *alpha = *beta;
    *beta  = a;
  } // now alpha >= beta >= gamma
  *A = ((*k21) - (*alpha)) * ((*k31) - (*alpha)) / ((*alpha) - (*beta)) / ((*alpha) - (*gamma))/(*v);
  *B = ((*k21) - (*beta)) * ((*k31) - (*beta)) / ((*beta) - (*alpha)) / ((*beta) - (*gamma))/(*v);
  *C = ((*k21) - (*gamma)) * ((*k31) - (*gamma)) / ((*gamma) - (*beta)) / ((*gamma) - (*alpha))/(*v);
  *vp  = (*v) * (*k12)/(*k21);
  *vp2 = (*v) * (*k13)/(*k31);
  *vss = (*v) + (*vp) + (*vp2);
  *cl  = (*v) * (*k10);
  *q   = (*v) * (*k12);
  *q2  = (*v) * (*k13);
  *Af  = (*A) * (*v);
  *Bf  = (*B) * (*v);
  *Cf  = (*C) * (*v);
  *t12alpha = M_LN2/(*alpha);
  *t12beta  = M_LN2/(*beta);
  *t12gamma = M_LN2/(*gamma);
}

SEXP toReal(SEXP in){
  int type = TYPEOF(in);
  if (type == REALSXP) return in;
  if (type == INTSXP) {
    SEXP ret = PROTECT(Rf_allocVector(REALSXP, Rf_length(in)));
    int *inI = INTEGER(in);
    double *retR = REAL(ret);
    for (int i = Rf_length(in); i--;){
      retR[i] = (double)(inI[i]);
    }
    UNPROTECT(1);
    return ret;
  }
  error(_("not an integer/real"));
  return R_NilValue;
}

SEXP derived1(int trans, SEXP inp, double dig) {
  double zer = 0;
  int lenP = Rf_length(VECTOR_ELT(inp, 0));
  int pro=0;
  SEXP tmp = PROTECT(toReal(VECTOR_ELT(inp, 0))); pro++;
  double *p1 = REAL(tmp);
  int lenV = Rf_length(VECTOR_ELT(inp, 1));
  tmp = PROTECT(toReal(VECTOR_ELT(inp, 1))); pro++;
  double *v1 = REAL(tmp);
  int lenOut = lenP;
  if (lenV != lenP){
    if (lenP == 1){
      lenOut = lenV;
    } else if (lenV != 1){
      error(_("The dimensions of the parameters must match"));
    }
  }
  // vc, kel, vss, cl, thalf, alpha, A, fracA
  SEXP ret  = PROTECT(allocVector(VECSXP, 8)); pro++;
  SEXP retN = PROTECT(allocVector(STRSXP, 8)); pro++;

  SET_STRING_ELT(retN,0,mkChar("vc"));
  SEXP vcS = PROTECT(allocVector(REALSXP, lenOut)); pro++;
  double *vc = REAL(vcS);
  SET_VECTOR_ELT(ret, 0, vcS);
  
  SET_STRING_ELT(retN,1,mkChar("kel"));
  SEXP kelS = PROTECT(allocVector(REALSXP, lenOut)); pro++;
  double *kel = REAL(kelS);
  SET_VECTOR_ELT(ret, 1, kelS);
  
  SET_STRING_ELT(retN,2,mkChar("vss"));
  SEXP vssS = PROTECT(allocVector(REALSXP, lenOut)); pro++;
  double *vss = REAL(vssS);
  SET_VECTOR_ELT(ret, 2, vssS);
  
  SET_STRING_ELT(retN,3,mkChar("cl"));
  SEXP clS = PROTECT(allocVector(REALSXP, lenOut)); pro++;
  double *cl = REAL(clS);
  SET_VECTOR_ELT(ret, 3, clS);
  
  SET_STRING_ELT(retN,4,mkChar("t12alpha"));
  SEXP thalfS = PROTECT(allocVector(REALSXP, lenOut)); pro++;
  double *thalf = REAL(thalfS);
  SET_VECTOR_ELT(ret, 4, thalfS);
  
  SET_STRING_ELT(retN,5,mkChar("alpha"));
  SEXP alphaS = PROTECT(allocVector(REALSXP, lenOut)); pro++;
  double *alpha = REAL(alphaS);
  SET_VECTOR_ELT(ret, 5, alphaS);
  
  SET_STRING_ELT(retN,6,mkChar("A"));
  SEXP AS = PROTECT(allocVector(REALSXP, lenOut)); pro++;
  double *A = REAL(AS);
  SET_VECTOR_ELT(ret, 6, AS);
  
  SET_STRING_ELT(retN,7,mkChar("fracA"));
  SEXP fracAS = PROTECT(allocVector(REALSXP, lenOut)); pro++;
  double *fracA = REAL(fracAS);
  SET_VECTOR_ELT(ret, 7, fracAS);

  SEXP sexp_class = PROTECT(allocVector(STRSXP, 1)); pro++;
  SET_STRING_ELT(sexp_class,0,mkChar("data.frame"));
  setAttrib(ret, R_ClassSymbol, sexp_class);

  SEXP sexp_rownames = PROTECT(allocVector(INTSXP,2)); pro++;
  INTEGER(sexp_rownames)[0] = NA_INTEGER;
  INTEGER(sexp_rownames)[1] = -lenOut;
  setAttrib(ret, R_RowNamesSymbol, sexp_rownames);

  setAttrib(ret, R_NamesSymbol, retN);

  unsigned int ncmta=0;

  for (int i = lenOut; i--;){
    parTrans(&trans, ((lenP == 1) ? p1 : p1++),
	     ((lenV == 1) ? v1 : v1++), &zer, &zer, &zer, &zer,
	     &ncmta, kel, vc, &zer, &zer, &zer, &zer);
    linCmtPar1(vc, kel, vss, cl, A, fracA, alpha, thalf);
    if (dig > 0){
      (*vc) = fprec((*vc), dig);
      (*kel) = fprec((*kel), dig);
      (*vss) = fprec((*vss), dig);
      (*cl) = fprec((*cl), dig);
      (*A) = fprec((*A), dig);
      (*alpha) = fprec((*alpha), dig);
      (*thalf) = fprec((*thalf), dig);
    }
    vc++; kel++; vss++; cl++; A++; fracA++; alpha++; thalf++;

  }
  UNPROTECT(pro);
  return ret;
}

SEXP derived2(int trans, SEXP inp, double dig) {
  double zer = 0;
  int pro=0;

  SEXP tmp = PROTECT(toReal(VECTOR_ELT(inp, 0))); pro++;
  int lenP1 = Rf_length(tmp);
  double *p1 = REAL(tmp);

  tmp = PROTECT(toReal(VECTOR_ELT(inp, 1))); pro++;
  int lenV = Rf_length(tmp);
  double *v1 = REAL(tmp);

  tmp = PROTECT(toReal(VECTOR_ELT(inp, 2))); pro++;
  int lenP2 = Rf_length(tmp);
  double *p2 = REAL(tmp);

  tmp = PROTECT(toReal(VECTOR_ELT(inp, 3))); pro++;
  int lenP3 = Rf_length(tmp);
  double *p3 = REAL(tmp);
  
  int lenOut = max2(lenV, lenP1);
  lenOut = max2(lenOut, lenP2);
  lenOut = max2(lenOut, lenP3);
  lenOut = max2(lenOut, lenV);
  if (lenOut != 1) {
    if ((lenP1 != 1 && lenP1 != lenOut) ||
	(lenP2 != 1 && lenP2 != lenOut) ||
	(lenP3 != 1 && lenP3 != lenOut) ||
	(lenV != 1  && lenV != lenOut)) {
      error(_("The dimensions of the parameters must match"));
    }
  }
  // vc, kel, k12, k21, vp, vss, cl, q, thalfAlpha, thalfBeta,
  // alpha, beta, A, B, fracA, fracB
  SEXP ret  = PROTECT(allocVector(VECSXP, 16)); pro++;
  SEXP retN = PROTECT(allocVector(STRSXP, 16)); pro++;

  SET_STRING_ELT(retN,0,mkChar("vc"));
  SEXP vcS = PROTECT(allocVector(REALSXP, lenOut)); pro++;
  double *vc = REAL(vcS);
  SET_VECTOR_ELT(ret, 0, vcS);
  
  SET_STRING_ELT(retN,1,mkChar("kel"));
  SEXP kelS = PROTECT(allocVector(REALSXP, lenOut)); pro++;
  double *kel = REAL(kelS);
  SET_VECTOR_ELT(ret, 1, kelS);

  SET_STRING_ELT(retN,2,mkChar("k12"));
  SEXP k12S = PROTECT(allocVector(REALSXP, lenOut)); pro++;
  double *k12 = REAL(k12S);
  SET_VECTOR_ELT(ret, 2, k12S);

  SET_STRING_ELT(retN,3,mkChar("k21"));
  SEXP k21S = PROTECT(allocVector(REALSXP, lenOut)); pro++;
  double *k21 = REAL(k21S);
  SET_VECTOR_ELT(ret, 3, k21S);

  SET_STRING_ELT(retN,4,mkChar("vp"));
  SEXP vpS = PROTECT(allocVector(REALSXP, lenOut)); pro++;
  double *vp = REAL(vpS);
  SET_VECTOR_ELT(ret, 4, vpS);

  SET_STRING_ELT(retN,5,mkChar("vss"));
  SEXP vssS = PROTECT(allocVector(REALSXP, lenOut)); pro++;
  double *vss = REAL(vssS);
  SET_VECTOR_ELT(ret, 5, vssS);

  SET_STRING_ELT(retN,6,mkChar("cl"));
  SEXP clS = PROTECT(allocVector(REALSXP, lenOut)); pro++;
  double *cl = REAL(clS);
  SET_VECTOR_ELT(ret, 6, clS);

  SET_STRING_ELT(retN,7,mkChar("q"));
  SEXP qS = PROTECT(allocVector(REALSXP, lenOut)); pro++;
  double *q = REAL(qS);
  SET_VECTOR_ELT(ret, 7, qS);

  SET_STRING_ELT(retN,8,mkChar("t12alpha"));
  SEXP thalfAlphaS = PROTECT(allocVector(REALSXP, lenOut)); pro++;
  double *thalfAlpha = REAL(thalfAlphaS);
  SET_VECTOR_ELT(ret, 8, thalfAlphaS);

  SET_STRING_ELT(retN,9,mkChar("t12beta"));
  SEXP thalfBetaS = PROTECT(allocVector(REALSXP, lenOut)); pro++;
  double *thalfBeta = REAL(thalfBetaS);
  SET_VECTOR_ELT(ret, 9, thalfBetaS);

  SET_STRING_ELT(retN,10,mkChar("alpha"));
  SEXP alphaS = PROTECT(allocVector(REALSXP, lenOut)); pro++;
  double *alpha = REAL(alphaS);
  SET_VECTOR_ELT(ret, 10, alphaS);

  SET_STRING_ELT(retN,11,mkChar("beta"));
  SEXP betaS = PROTECT(allocVector(REALSXP, lenOut)); pro++;
  double *beta = REAL(betaS);
  SET_VECTOR_ELT(ret, 11, betaS);

  SET_STRING_ELT(retN,12,mkChar("A"));
  SEXP AS = PROTECT(allocVector(REALSXP, lenOut)); pro++;
  double *A = REAL(AS);
  SET_VECTOR_ELT(ret, 12, AS);

  SET_STRING_ELT(retN,13,mkChar("B"));
  SEXP BS = PROTECT(allocVector(REALSXP, lenOut)); pro++;
  double *B = REAL(BS);
  SET_VECTOR_ELT(ret, 13, BS);

  SET_STRING_ELT(retN,14,mkChar("fracA"));
  SEXP fracAS = PROTECT(allocVector(REALSXP, lenOut)); pro++;
  double *fracA = REAL(fracAS);
  SET_VECTOR_ELT(ret, 14, fracAS);

  SET_STRING_ELT(retN,15,mkChar("fracB"));
  SEXP fracBS = PROTECT(allocVector(REALSXP, lenOut)); pro++;
  double *fracB = REAL(fracBS);
  SET_VECTOR_ELT(ret, 15, fracBS);

  SEXP sexp_class = PROTECT(allocVector(STRSXP, 1)); pro++;
  SET_STRING_ELT(sexp_class,0,mkChar("data.frame"));
  setAttrib(ret, R_ClassSymbol, sexp_class);

  SEXP sexp_rownames = PROTECT(allocVector(INTSXP,2)); pro++;
  INTEGER(sexp_rownames)[0] = NA_INTEGER;
  INTEGER(sexp_rownames)[1] = -lenOut;
  setAttrib(ret, R_RowNamesSymbol, sexp_rownames);

  setAttrib(ret, R_NamesSymbol, retN);

  unsigned int ncmta=0;

  for (int i = lenOut; i--;){
    parTrans(&trans, ((lenP1 == 1) ? p1 : p1++), ((lenV == 1) ? v1 : v1++),
	     ((lenP2 == 1) ? p2 : p2++), ((lenP3 == 1) ? p3 : p3++), &zer, &zer,
	     &ncmta, kel, vc, k12, k21, &zer, &zer);
    linCmtPar2(vc, kel, k12, k21, vp, vss, cl, q, A, B, fracA, fracB,
	       alpha, beta, thalfAlpha, thalfBeta);
    if (dig > 0){
      (*vc) = fprec((*vc), dig);
      (*kel) = fprec((*kel), dig);
      (*k12) = fprec((*k12), dig);
      (*k21) = fprec((*k21), dig);
      (*vp) = fprec((*vp), dig);
      (*vss) = fprec((*vss), dig);
      (*cl) = fprec((*cl), dig);
      (*q) = fprec((*q), dig);
      (*A) = fprec((*A), dig);
      (*B) = fprec((*B), dig);
      (*fracA) = fprec((*fracA), dig);
      (*fracB) = fprec((*fracB), dig);
      (*alpha) = fprec((*alpha), dig);
      (*beta) = fprec((*beta), dig);
      (*thalfAlpha) = fprec((*thalfAlpha), dig);
      (*thalfBeta) = fprec((*thalfBeta), dig);
    }
    vc++; kel++; k12++; k21++; vp++; vss++; cl++; q++;
    A++; B++; fracA++; fracB++; alpha++; beta++;
    thalfAlpha++; thalfBeta++;
  }
  UNPROTECT(pro);
  return ret;
}

SEXP derived3(int trans, SEXP inp, double dig) {
  int pro = 0;
  SEXP tmp = PROTECT(toReal(VECTOR_ELT(inp, 0))); pro++;
  int lenP1 = Rf_length(tmp);
  double *p1 = REAL(tmp);

  tmp = PROTECT(toReal(VECTOR_ELT(inp, 1))); pro++;
  int lenV = Rf_length(tmp);
  double *v1 = REAL(tmp);

  tmp = PROTECT(toReal(VECTOR_ELT(inp, 2))); pro++;
  int lenP2 = Rf_length(tmp);
  double *p2 = REAL(tmp);

  tmp = PROTECT(toReal(VECTOR_ELT(inp, 3))); pro++;
  int lenP3 = Rf_length(tmp);
  double *p3 = REAL(tmp);

  tmp = PROTECT(toReal(VECTOR_ELT(inp, 4))); pro++;
  int lenP4 = Rf_length(tmp);
  double *p4 = REAL(tmp);

  tmp = PROTECT(toReal(VECTOR_ELT(inp, 5))); pro++;
  int lenP5 = Rf_length(tmp);
  double *p5 = REAL(tmp);
  
  int lenOut = max2(lenV, lenP1);
  lenOut = max2(lenOut, lenP2);
  lenOut = max2(lenOut, lenP3);
  lenOut = max2(lenOut, lenV);
  lenOut = max2(lenOut, lenP4);
  lenOut = max2(lenOut, lenP5);
  if (lenOut != 1) {
    if ((lenP1 != 1 && lenP1 != lenOut) ||
	(lenP2 != 1 && lenP2 != lenOut) ||
	(lenP3 != 1 && lenP3 != lenOut) ||
	(lenP4 != 1 && lenP4 != lenOut) ||
	(lenP5 != 1 && lenP5 != lenOut) ||
	(lenV != 1  && lenV != lenOut)) {
      error(_("The dimensions of the parameters must match"));
    }
  }
  // vc, kel, k12, k21, vp, vss, cl, q, thalfAlpha, thalfBeta,
  // alpha, beta, A, B, fracA, fracB
  SEXP ret  = PROTECT(allocVector(VECSXP, 24)); pro++;
  SEXP retN = PROTECT(allocVector(STRSXP, 24)); pro++;

  SET_STRING_ELT(retN,0,mkChar("vc"));
  SEXP vcS = PROTECT(allocVector(REALSXP, lenOut)); pro++;
  double *vc = REAL(vcS);
  SET_VECTOR_ELT(ret, 0, vcS);
  
  SET_STRING_ELT(retN,1,mkChar("kel"));
  SEXP kelS = PROTECT(allocVector(REALSXP, lenOut)); pro++;
  double *kel = REAL(kelS);
  SET_VECTOR_ELT(ret, 1, kelS);

  SET_STRING_ELT(retN,2,mkChar("k12"));
  SEXP k12S = PROTECT(allocVector(REALSXP, lenOut)); pro++;
  double *k12 = REAL(k12S);
  SET_VECTOR_ELT(ret, 2, k12S);

  SET_STRING_ELT(retN,3,mkChar("k21"));
  SEXP k21S = PROTECT(allocVector(REALSXP, lenOut)); pro++;
  double *k21 = REAL(k21S);
  SET_VECTOR_ELT(ret, 3, k21S);

  SET_STRING_ELT(retN,4,mkChar("k13"));
  SEXP k13S = PROTECT(allocVector(REALSXP, lenOut)); pro++;
  double *k13 = REAL(k13S);
  SET_VECTOR_ELT(ret, 4, k13S);

  SET_STRING_ELT(retN,5,mkChar("k31"));
  SEXP k31S = PROTECT(allocVector(REALSXP, lenOut)); pro++;
  double *k31 = REAL(k31S);
  SET_VECTOR_ELT(ret, 5, k31S);

  SET_STRING_ELT(retN,6,mkChar("vp"));
  SEXP vpS = PROTECT(allocVector(REALSXP, lenOut)); pro++;
  double *vp = REAL(vpS);
  SET_VECTOR_ELT(ret, 6, vpS);

  SET_STRING_ELT(retN,7,mkChar("vp2"));
  SEXP vp2S = PROTECT(allocVector(REALSXP, lenOut)); pro++;
  double *vp2 = REAL(vp2S);
  SET_VECTOR_ELT(ret, 7, vp2S);

  SET_STRING_ELT(retN,8,mkChar("vss"));
  SEXP vssS = PROTECT(allocVector(REALSXP, lenOut)); pro++;
  double *vss = REAL(vssS);
  SET_VECTOR_ELT(ret, 8, vssS);

  SET_STRING_ELT(retN,9,mkChar("cl"));
  SEXP clS = PROTECT(allocVector(REALSXP, lenOut)); pro++;
  double *cl = REAL(clS);
  SET_VECTOR_ELT(ret, 9, clS);

  SET_STRING_ELT(retN,10,mkChar("q"));
  SEXP qS = PROTECT(allocVector(REALSXP, lenOut)); pro++;
  double *q = REAL(qS);
  SET_VECTOR_ELT(ret, 10, qS);

  SET_STRING_ELT(retN,11,mkChar("q2"));
  SEXP q2S = PROTECT(allocVector(REALSXP, lenOut)); pro++;
  double *q2 = REAL(q2S);
  SET_VECTOR_ELT(ret, 11, q2S);

  SET_STRING_ELT(retN,12,mkChar("t12alpha"));
  SEXP thalfAlphaS = PROTECT(allocVector(REALSXP, lenOut)); pro++;
  double *thalfAlpha = REAL(thalfAlphaS);
  SET_VECTOR_ELT(ret, 12, thalfAlphaS);

  SET_STRING_ELT(retN,13,mkChar("t12beta"));
  SEXP thalfBetaS = PROTECT(allocVector(REALSXP, lenOut)); pro++;
  double *thalfBeta = REAL(thalfBetaS);
  SET_VECTOR_ELT(ret, 13, thalfBetaS);

  SET_STRING_ELT(retN,14,mkChar("t12gamma"));
  SEXP thalfGammaS = PROTECT(allocVector(REALSXP, lenOut)); pro++;
  double *thalfGamma = REAL(thalfGammaS);
  SET_VECTOR_ELT(ret, 14, thalfGammaS);

  SET_STRING_ELT(retN,15,mkChar("alpha"));
  SEXP alphaS = PROTECT(allocVector(REALSXP, lenOut)); pro++;
  double *alpha = REAL(alphaS);
  SET_VECTOR_ELT(ret, 15, alphaS);

  SET_STRING_ELT(retN,16,mkChar("beta"));
  SEXP betaS = PROTECT(allocVector(REALSXP, lenOut)); pro++;
  double *beta = REAL(betaS);
  SET_VECTOR_ELT(ret, 16, betaS);

  SET_STRING_ELT(retN,17,mkChar("gamma"));
  SEXP gammaS = PROTECT(allocVector(REALSXP, lenOut)); pro++;
  double *gamma = REAL(gammaS);
  SET_VECTOR_ELT(ret, 17, gammaS);

  SET_STRING_ELT(retN,18,mkChar("A"));
  SEXP AS = PROTECT(allocVector(REALSXP, lenOut)); pro++;
  double *A = REAL(AS);
  SET_VECTOR_ELT(ret, 18, AS);


  SET_STRING_ELT(retN,19,mkChar("B"));
  SEXP BS = PROTECT(allocVector(REALSXP, lenOut)); pro++;
  double *B = REAL(BS);
  SET_VECTOR_ELT(ret, 19, BS);

  SET_STRING_ELT(retN,20,mkChar("C"));
  SEXP CS = PROTECT(allocVector(REALSXP, lenOut)); pro++;
  double *C = REAL(CS);
  SET_VECTOR_ELT(ret, 20, CS);

  SET_STRING_ELT(retN,21,mkChar("fracA"));
  SEXP fracAS = PROTECT(allocVector(REALSXP, lenOut)); pro++;
  double *fracA = REAL(fracAS);
  SET_VECTOR_ELT(ret, 21, fracAS);

  SET_STRING_ELT(retN,22,mkChar("fracB"));
  SEXP fracBS = PROTECT(allocVector(REALSXP, lenOut)); pro++;
  double *fracB = REAL(fracBS);
  SET_VECTOR_ELT(ret, 22, fracBS);

  SET_STRING_ELT(retN,23,mkChar("fracC"));
  SEXP fracCS = PROTECT(allocVector(REALSXP, lenOut)); pro++;
  double *fracC = REAL(fracCS);
  SET_VECTOR_ELT(ret, 23, fracCS);

  SEXP sexp_class = PROTECT(allocVector(STRSXP, 1)); pro++;
  SET_STRING_ELT(sexp_class,0,mkChar("data.frame"));
  setAttrib(ret, R_ClassSymbol, sexp_class);

  SEXP sexp_rownames = PROTECT(allocVector(INTSXP,2)); pro++;
  INTEGER(sexp_rownames)[0] = NA_INTEGER;
  INTEGER(sexp_rownames)[1] = -lenOut;
  setAttrib(ret, R_RowNamesSymbol, sexp_rownames);

  setAttrib(ret, R_NamesSymbol, retN);

  unsigned int ncmta=0;

  for (int i = lenOut; i--;){
    parTrans(&trans, ((lenP1 == 1) ? p1 : p1++), ((lenV == 1) ? v1 : v1++),
	     ((lenP2 == 1) ? p2 : p2++), ((lenP3 == 1) ? p3 : p3++),
	     ((lenP4 == 1) ? p4 : p4++), ((lenP5 == 1) ? p5 : p5++),
	     &ncmta, kel, vc, k12, k21, k13, k31);
    linCmtPar3(vc, kel, k12, k21, k13, k31,
	       vp, vp2, vss, cl, q, q2,
	       A, B, C, fracA, fracB, fracC, alpha, beta, gamma,
	       thalfAlpha, thalfBeta, thalfGamma);
    if (dig > 0) {
      (*vc)  = fprec((*vc), dig);
      (*kel) = fprec((*kel), dig);
      (*k12) = fprec((*k12), dig);
      (*k21) = fprec((*k21), dig);
      (*k13) = fprec((*k13), dig);
      (*k31) = fprec((*k31), dig);
      (*vp)  = fprec((*vp), dig);
      (*vss) = fprec((*vss), dig);
      (*vp2) = fprec((*vp2), dig);
      (*cl)  = fprec((*cl), dig);
      (*q)   = fprec((*q), dig);
      (*q2)  = fprec((*q2), dig);
      (*A)   = fprec((*A), dig);
      (*B)   = fprec((*B), dig);
      (*C)   = fprec((*C), dig);
      (*fracA)=fprec((*fracA), dig);
      (*fracB)=fprec((*fracB), dig);
      (*fracC)=fprec((*fracC), dig);
      (*alpha)=fprec((*alpha), dig);
      (*beta) =fprec((*beta), dig);
      (*gamma)=fprec((*gamma), dig);
      (*thalfAlpha)=fprec((*thalfAlpha), dig);
      (*thalfBeta)=fprec((*thalfBeta), dig);
      (*thalfGamma)=fprec((*thalfGamma), dig);
    }
    vc++; kel++; k12++; k21++; k13++; k31++;
    vp++; vp2++; vss++; cl++; q++; q2++;
    A++; B++; C++; fracA++; fracB++; fracC++; alpha++; beta++; gamma++;
    thalfAlpha++; thalfBeta++; thalfGamma++;
  }
  UNPROTECT(pro);
  return ret;
}

SEXP _calcDerived(SEXP ncmtSXP, SEXP transSXP, SEXP inp, SEXP sigdigSXP) {
  int tInp = TYPEOF(inp);
  int trans=-1;
  if (TYPEOF(transSXP) == REALSXP){
    trans = (int)(REAL(transSXP)[0]);
  }
  int ncmt=-1;
  if (TYPEOF(ncmtSXP) == REALSXP) {
    ncmt = (int)(REAL(ncmtSXP)[0]);
  }
  double dig=0.0;
  int tDig = TYPEOF(sigdigSXP);
  if (tDig == INTSXP) {
    dig = (double)(INTEGER(sigdigSXP)[0]);
  } else if (tDig == REALSXP) {
    dig = REAL(sigdigSXP)[0];
  }
  if (tInp == VECSXP){
    switch (ncmt){
    case 1:
      return derived1(trans, inp, dig);
      break;
    case 2:
      return derived2(trans, inp, dig);
      break;
    case 3:
      return derived3(trans, inp, dig);
      break;
    default:
      error(_("'ncmt' needs to be 1-3"));
    }
  } else {
    error(_("'inp' needs to be list/data frame"));
  }
  return R_NilValue;
}

double linCmtA(rx_solve *rx, unsigned int id, double t, int linCmt,
	       int i_cmt, int trans,
	       double p1, double v1,
	       double p2, double p3,
	       double p4, double p5,
	       double d_tlag, double d_F, double d_rate1, double d_dur1,
	       // Oral parameters
	       double d_ka, double d_tlag2, double d_F2,  double d_rate2, double d_dur2) {
  /* REprintf("F: %f; F2: %f\n", d_F, d_F2); */
  rx_solving_options_ind *ind = &(rx->subjects[id]);
  int evid, wh, cmt, wh100, whI, wh0;
  /* evid = ind->evid[ind->ix[ind->idx]]; */
  /* if (evid) REprintf("evid0[%d:%d]: %d; curTime: %f\n", id, ind->idx, evid, t); */
  int idx = ind->idx;
  double Alast0[4] = {0, 0, 0, 0};
  sortIfNeeded(rx, ind, id, &linCmt, &d_tlag, &d_tlag2, &d_F, &d_F2,
	       &d_rate1, &d_dur1, &d_rate2, &d_dur2);
  rx_solving_options *op = rx->op;
  int oral0;
  oral0 = (d_ka > 0) ? 1 : 0;
  double *A;
  double *Alast;
  /* A = Alast0; Alast=Alast0; */
  double tlast;
  double it = getTime(ind->ix[idx], ind);
  double curTime=0.0;
  
  if (t != it) {
    // Try to get another idx by bisection
    /* REprintf("it pre: %f", it); */
    idx = _locateTimeIndex(t, ind);
    it = getTime(ind->ix[idx], ind);
    /* REprintf("it post: %f", it); */
  }
  /* REprintf("idx: %d; solved: %d; t: %f fabs: %f\n", idx, ind->solved[idx], t, fabs(t-it)); */
  int sameTime = fabs(t-it) < sqrt(DOUBLE_EPS);
  if (idx <= ind->solved && sameTime){
    // Pull from last solved value (cached)
    A = ind->linCmtAdvan+(op->nlin)*idx;
    if (trans == 10) {
      return(A[oral0]*(v1+p3+p5));
    } else {
      return(A[oral0]/v1);
    }
  }
  unsigned int ncmt = 1;
  double rx_k=0, rx_v=0;
  double rx_k12=0;
  double rx_k21=0;
  double rx_k13=0;
  double rx_k31=0;
  double *rate = ind->linCmtRate;
  double b1=0, b2=0, r1 = 0, r2 = 0;
  A = Alast0;
  if (idx <= ind->solved){
    if (!parTrans(&trans, &p1, &v1, &p2, &p3, &p4, &p5,
		  &ncmt, &rx_k, &rx_v, &rx_k12,
		  &rx_k21, &rx_k13, &rx_k31)){
      REprintf(_("invalid translation\n"));
      return NA_REAL;
    }
  } else {
    A = ind->linCmtAdvan+(op->nlin)*idx;
    if (idx == 0) {
      Alast = Alast0;
      tlast = getTime(ind->ix[0], ind);
    } else {
      tlast = getTime(ind->ix[idx-1], ind);
      Alast = ind->linCmtAdvan+(op->nlin)*(idx-1);
    }
    curTime = getTime(ind->ix[idx], ind);
    if (!parTrans(&trans, &p1, &v1, &p2, &p3, &p4, &p5,
		  &ncmt, &rx_k, &rx_v, &rx_k12,
		  &rx_k21, &rx_k13, &rx_k31)){
      REprintf(_("invalid translation\n"));
      return NA_REAL;
    }
    evid = ind->evid[ind->ix[idx]];
    /* if (evid) REprintf("evid: %d; curTime: %f\n",evid, curTime); */
    if (op->nlinR == 2){
      r1 = rate[0];
      r2 = rate[1];
    } else {
      r1 = rate[0];
    }
    int doMultiply = 0, doReplace=0;
    double amt=0;
    double rateAdjust= 0;
    int doRate=0;
    int extraAdvan = 1;
    wh0 = 0;
    if (isObs(evid)){
      // Only apply doses when you need to set the solved system.
      // When it is an observation of course you do not need to apply the doses
    } else if (evid == 3){
      // Reset event
      Alast=Alast0;
    } else {
      getWh(evid, &wh, &cmt, &wh100, &whI, &wh0);
      /* REprintf("evid: %d; cmt: %d; %d\n", evid, cmt, linCmt); */
      if (!oral0 && cmt > linCmt) {
	int foundBad = 0;
	for (int j = 0; j < ind->nBadDose; j++){
	  if (ind->BadDose[j] == cmt+1){
	    foundBad=1;
	    break;
	  }
	}
	if (!foundBad){
	  ind->BadDose[ind->nBadDose]=cmt+1;
	  ind->nBadDose++;
	}
      } else if (oral0 && cmt > linCmt+1) {
	int foundBad = 0;
	for (int j = 0; j < ind->nBadDose; j++){
	  if (ind->BadDose[j] == cmt+1){
	    foundBad=1;
	    break;
	  }
	}
	if (!foundBad){
	  ind->BadDose[ind->nBadDose]=cmt+1;
	  ind->nBadDose++;
	}
      }
      int cmtOff = cmt-linCmt;
      if ((oral0 && cmtOff > 1) ||
	  (!oral0 && cmtOff != 0)) {
      } else {
	syncIdx(ind);
	amt = ind->dose[ind->ixds];
	if (!ISNA(amt) && (amt > 0) && (wh0 == 10 || wh0 == 20)) {
	  // dosing to cmt
	  // Steady state doses; wh0 == 20 is equivalent to SS=2 in NONMEM
	  double tau = ind->ii[ind->ixds];
	  // For now advance based solving to steady state (just like ODE)
	  double aSave[4];
	  if (wh0 == 20) {
	    doAdvan(A, Alast, tlast, // Time of last amounts
		    curTime, ncmt, oral0, &b1, &b2, &r1, &r2,
		    &d_ka, &rx_k, &rx_k12, &rx_k21,
		    &rx_k13, &rx_k31);
	    for (int i = ncmt + oral0; i--;){
	      aSave[i] = A[i];
	    }
	  }
	  // Reset all the rates
	  if (op->nlinR == 2){
	    rate[0]=0.0;
	    rate[1]=0.0;
	    r1=0; r2=0;
	  } else {
	    rate[0] = 0.0;
	    r1=0; r2=0;
	  }
	  Alast = Alast0;
	  tlast = 0;
	  curTime = tau;
	  double tinf, r0;
	  int doInf=0;
	  switch (whI){
	  case 0: {
	    // Get bolus dose
	    if (cmtOff == 0){
	      b1 = amt*d_F;
	    } else {
	      b2 = amt*d_F2;
	    }
	    ssTau(A, ncmt, oral0, &tau, &b1, &b2, &d_ka,
		  &rx_k, &rx_k12, &rx_k21, &rx_k13, &rx_k31);
	  } break;
	  case 8: // Duration is modeled
	  case 9: { // Rate is modeled
	    if (whI == 9) {
	      if (cmtOff == 0)  {
		// Infusion to central compartment with oral dosing
		r0 = d_rate1;
		tinf = amt/r0*d_F;
	      } else {
		// Infusion to central compartment or depot
		r0 = d_rate2;
		tinf = amt/r0*d_F2;
	      }
	    } else {
	      // duration is modeled
	      if (cmtOff == 0) {
		// With oral dosing infusion to central compartment
		tinf = d_dur1;
		r0 = amt/tinf*d_F;
	      } else {
		// Infusion to compartment #1 or depot
		tinf = d_dur2;
		r0 = amt/tinf*d_F2;
	      }
	    }
	    doInf=1;
	  } break;
	  case 1:
	  case 2: {
	    if (ISNA(amt)){
	    } else if (amt > 0) {
	      doInf=1;
	      unsigned int p;
	      r0 = amt;
	      tinf = _getDur(ind->ixds, ind, 1, &p);
	      if (whI == 1){
		// Duration changes
		if (cmtOff == 0){
		  tinf *= d_F;
		} else {
		  tinf *= d_F2;
		}
	      } else {
		// Rate changes
		if (cmtOff == 0){
		  r0 *= d_F;
		} else {
		  r0 *= d_F2;
		}
	      }
	    }
	  }
	  }
	  if (doInf){
	    // Infusion steady state
	    if (tinf >= tau){
	      ind->wrongSSDur=1;
	      for (int i = ncmt + oral0; i--;){
		A[i] += R_NaN;
	      }
	      extraAdvan=0;
	    } else {
	      if (cmtOff == 0){
		r1 = r0; r2 = 0;
	      } else {
		r1 = 0; r2 = r0;
	      }
	      ssRateTau(A, ncmt, oral0, &tinf, &tau,
			&r1, &r2, &d_ka,
			&rx_k, &rx_k12, &rx_k21, &rx_k13, &rx_k31);
	    }
	  }
	  // Now calculate steady state
	  if (wh0 == 20) {
	    for (int i = ncmt + oral0; i--;){
	      A[i] += aSave[i];
	    }
	  }
	  extraAdvan=0;
	} else if (wh0 == 30) {
	  // Turning off a compartment; Not supported put everything to NaN
	  for (int i = ncmt + oral0; i--;){
	    A[i] = R_NaN;
	  }
	  extraAdvan=0;
	}
	// dosing to cmt
	amt = ind->dose[ind->ixds];
	switch (whI){
	case 0: { // Bolus dose
	  // base dose
	  if (cmtOff == 0){
	    b1 = amt*d_F;
	  } else {
	    b2 = amt*d_F2;
	  }
	} break;
	case 4: {
	  doReplace = cmtOff+1;
	} break;
	case 5: {
	  doMultiply= cmtOff+1;
	} break;
	case 9: { // modeled rate.
	  // These are specified in the linCmt
	  if (cmtOff == 0)  {
	    // Infusion to central compartment with oral dosing
	    rateAdjust = d_rate1;
	  } else {
	    // Infusion to central compartment or depot
	    rateAdjust = d_rate2;
	  }
	  // Save rate turn off in next dose
	  doRate = cmtOff+1;
	} break;
	case 8: { // modeled duration. 
	  //InfusionRate[cmt] -= dose[ind->ixds+1];
	  if (cmtOff == 0) {
	    // With oral dosing infusion to central compartment
	    rateAdjust = amt/d_dur1*d_F;
	  } else {
	    // Infusion to compartment #1 or depot
	    rateAdjust = amt/d_dur2*d_F2;
	  }
	  doRate = cmtOff+1;
	} break;
	case 7:{ // End modeled rate
	  if (cmtOff == 0)  {
	    // Infusion to central compartment with oral dosing
	    rateAdjust = -d_rate1;
	  } else {
	    // Infusion to central compartment or depot
	    rateAdjust = -d_rate2;
	  }
	  doRate = cmtOff+1;
	} break;
	case 1: { // Begin infusion
	  rateAdjust = amt; // Amt is negative when turning off
	  doRate = cmtOff+1;
	} break;
	case 6: { // end modeled duration
	  if (cmtOff == 0) {
	    // With oral dosing infusion to central compartment
	    rateAdjust = -amt/d_dur1*d_F;
	  } else {
	    // Infusion to compartment #1 or depot
	    rateAdjust = -amt/d_dur2*d_F2;
	  }
	  doRate = cmtOff+1;
	} break;
	case 2: {
	  // In this case bio-availability changes the rate, but the duration remains constant.
	  // rate = amt/dur
	  if (cmtOff == 0){
	    rateAdjust = amt*d_F;
	  } else {
	    rateAdjust = amt*d_F2;
	  }
	  doRate = cmtOff+1;
	} break;
	}
	if (wh0 == 40){
	  // Steady state infusion
	  // Now advance to steady state dosing
	  // These are easy to solve
	  if (op->nlinR == 2){
	    rate[0]=0.0;
	    rate[1]=0.0;
	  } else {
	    rate[0] = 0.0;
	  }
	  if (doRate == 1){
	    r1 = rateAdjust;
	    r2 = 0;
	  } else {
	    r1 = 0;
	    r2 = rateAdjust;
	  }
	  doRate=0;
	  ssRate(A, ncmt, oral0, &r1, &r2,
		 &d_ka, &rx_k, &rx_k12, &rx_k21,
		 &rx_k13, &rx_k31);
	  extraAdvan=0;
	}    

      }
    }
    /* REprintf("evid: %d; wh: %d; cmt: %d; wh100: %d; whI: %d; wh0: %d; %f\n", */
    /* 	   evid, wh, cmt, wh100, whI, wh0, A[oral0]); */
    /* REprintf("curTime: t:%f, it: %f curTime:%f, tlast: %f, b1: %f ", t, it, curTime, tlast, b1); */
    if (extraAdvan){
      doAdvan(A, Alast, tlast, // Time of last amounts
	      curTime, ncmt, oral0, &b1, &b2, &r1, &r2,
	      &d_ka, &rx_k, &rx_k12, &rx_k21,
	      &rx_k13, &rx_k31);
    }
    if (doReplace){
      A[doReplace-1] = amt;
    } else if (doMultiply){
      A[doMultiply-1] *= amt;
    } else if (doRate){
      rate[doRate-1] += rateAdjust;
    } 
    ind->solved = idx;
  }
  if (!sameTime){
    // Compute the advan solution of a t outside of the mesh.
    Alast = A;
    A = Alast0;
    tlast = curTime;
    curTime = t;
    b1 = b2 = 0;
    doAdvan(A, Alast, tlast, // Time of last amounts
	    curTime, ncmt, oral0, &b1, &b2, &r1, &r2,
	    &d_ka, &rx_k, &rx_k12, &rx_k21,
	    &rx_k13, &rx_k31);
  }
  /* REprintf("t: %f %f %d %d\n", t, A[oral0], idx, ind->ix[idx]); */
  /* REprintf("%f,%f,%f\n", A[oral0], rx_v, A[oral0]/rx_v); */
  return A[oral0]/rx_v;
}


double linCmtC(rx_solve *rx, unsigned int id, double t, int linCmt,
	       int i_cmt, int trans,
	       double p1, double v1,
	       double p2, double p3,
	       double p4, double p5,
	       double d_tlag, double d_F, double d_rate1, double d_dur1,
	       // Oral parameters
	       double d_ka, double d_tlag2, double d_F2,  double d_rate2, double d_dur2) {
  rx_solving_options_ind *ind = &(rx->subjects[id]);
  int evid, wh, cmt, wh100, whI, wh0;
  int idxF = ind->idx;
  double Alast[4] = {0, 0, 0, 0};
  double A[4]     = {0, 0, 0, 0};
  sortIfNeeded(rx, ind, id, &linCmt, &d_tlag, &d_tlag2, &d_F, &d_F2,
	       &d_rate1, &d_dur1, &d_rate2, &d_dur2);
  rx_solving_options *op = rx->op;
  int oral0;
  oral0 = (d_ka > 0) ? 1 : 0;
  double it = getTime(ind->ix[idxF], ind);
  double curTime, tlast;
  curTime = tlast = getTime(ind->ix[0], ind);

  if (t != it) {
    // Try to get another idx by bisection
    /* REprintf("it pre: %f", it); */
    idxF = _locateTimeIndex(t, ind);
    it = getTime(ind->ix[idxF], ind);
    /* REprintf("it post: %f", it); */
  }
  unsigned int ncmt = 1;
  double rx_k=0, rx_v=0;
  double rx_k12=0;
  double rx_k21=0;
  double rx_k13=0;
  double rx_k31=0;
  double rate[2] = {0, 0};
  double b1=0, b2=0, r1 = 0, r2 = 0;
  int oldIxds = ind->ixds, oldIdx=ind->idx;
  if (!parTrans(&trans, &p1, &v1, &p2, &p3, &p4, &p5,
		&ncmt, &rx_k, &rx_v, &rx_k12,
		&rx_k21, &rx_k13, &rx_k31)){
    REprintf(_("invalid translation\n"));
    return NA_REAL;
  } else {
    ind->ixds = 0;
    
    int doMultiply = 0, doReplace=0;
    double amt=0;
    double rateAdjust= 0;
    int doRate=0, doInf=0;
    int extraAdvan = 1;
    for (int idx = 0; idx <= idxF; idx++) {
      ind->idx = idx;
      curTime = getTime(ind->ix[idx], ind);
      evid = ind->evid[ind->ix[idx]];
      /* if (evid) REprintf("evid: %d; curTime: %f\n",evid, curTime); */
      wh0 = 0;
      if (isObs(evid)){
	if (idx != idxF){
	  continue;
	}
      } else if (evid == 3){
	// Reset event
	ind->ixds++;
	for (int i = oral0+ncmt; i--;){
	  Alast[i] = 0.0;
	}
      } else {
	getWh(evid, &wh, &cmt, &wh100, &whI, &wh0);
	/* REprintf("evid: %d; cmt: %d; %d\n", evid, cmt, linCmt); */
	if (!oral0 && cmt > linCmt) {
	  int foundBad = 0;
	  for (int j = 0; j < ind->nBadDose; j++){
	    if (ind->BadDose[j] == cmt+1){
	      foundBad=1;
	      break;
	    }
	  }
	  if (!foundBad){
	    ind->BadDose[ind->nBadDose]=cmt+1;
	    ind->nBadDose++;
	  }
	} else if (oral0 && cmt > linCmt+1) {
	  int foundBad = 0;
	  for (int j = 0; j < ind->nBadDose; j++){
	    if (ind->BadDose[j] == cmt+1){
	      foundBad=1;
	      break;
	    }
	  }
	  if (!foundBad){
	    ind->BadDose[ind->nBadDose]=cmt+1;
	    ind->nBadDose++;
	  }
	}
	int cmtOff = cmt-linCmt;
	if ((oral0 && cmtOff > 1) ||
	    (!oral0 && cmtOff != 0)) {
	} else {
	  syncIdx(ind);
	  amt = ind->dose[ind->ixds];
	  if (!ISNA(amt) && (amt > 0) && (wh0 == 10 || wh0 == 20)) {
	    // dosing to cmt
	    // Steady state doses; wh0 == 20 is equivalent to SS=2 in NONMEM
	    double tau = ind->ii[ind->ixds];
	    // For now advance based solving to steady state (just like ODE)
	    double aSave[4];
	    if (wh0 == 20) {
	      doAdvan(A, Alast, tlast, // Time of last amounts
		      curTime, ncmt, oral0, &b1, &b2, &r1, &r2,
		      &d_ka, &rx_k, &rx_k12, &rx_k21,
		      &rx_k13, &rx_k31);
	      for (int i = ncmt + oral0; i--;){
		aSave[i] = A[i];
	      }
	    }
	    // Reset all the rates
	    if (op->nlinR == 2){
	      rate[0]=0.0;
	      rate[1]=0.0;
	      r1=0; r2=0;
	    } else {
	      rate[0] = 0.0;
	      r1=0; r2=0;
	    }
	    for (int i = ncmt + oral0; i--;){
	      Alast[i] = 0.0;
	    }
	    tlast = 0;
	    curTime = tau;
	    double tinf, r0;
	    int doInf=0;
	    switch (whI){
	    case 0: {
	      // Get bolus dose
	      if (cmtOff == 0){
		b1 = amt*d_F;
	      } else {
		b2 = amt*d_F2;
	      }
	      ssTau(A, ncmt, oral0, &tau, &b1, &b2, &d_ka,
		    &rx_k, &rx_k12, &rx_k21, &rx_k13, &rx_k31);
	    } break;
	    case 8: // Duration is modeled
	    case 9: { // Rate is modeled
	      if (whI == 9) {
		if (cmtOff == 0)  {
		  // Infusion to central compartment with oral dosing
		  r0 = d_rate1;
		  tinf = amt/r0*d_F;
		} else {
		  // Infusion to central compartment or depot
		  r0 = d_rate2;
		  tinf = amt/r0*d_F2;
		}
	      } else {
		// duration is modeled
		if (cmtOff == 0) {
		  // With oral dosing infusion to central compartment
		  tinf = d_dur1;
		  r0 = amt/tinf*d_F;
		} else {
		  // Infusion to compartment #1 or depot
		  tinf = d_dur2;
		  r0 = amt/tinf*d_F2;
		}
	      }
	      doInf=1;
	    } break;
	    case 1:
	    case 2: {
	      if (ISNA(amt)){
	      } else if (amt > 0) {
		doInf=1;
		unsigned int p;
		r0 = amt;
		tinf = _getDur(ind->ixds, ind, 1, &p);
		if (whI == 1){
		  // Duration changes
		  if (cmtOff == 0){
		    tinf *= d_F;
		  } else {
		    tinf *= d_F2;
		  }
		} else {
		  // Rate changes
		  if (cmtOff == 0){
		    r0 *= d_F;
		  } else {
		    r0 *= d_F2;
		  }
		}
	      }
	    }
	    }
	    if (doInf){
	      // Infusion steady state
	      if (tinf >= tau){
		ind->wrongSSDur=1;
		for (int i = ncmt + oral0; i--;){
		  A[i] += R_NaN;
		}
		extraAdvan=0;
	      } else {
		if (cmtOff == 0){
		  r1 = r0; r2 = 0;
		} else {
		  r1 = 0; r2 = r0;
		}
		ssRateTau(A, ncmt, oral0, &tinf, &tau,
			  &r1, &r2, &d_ka,
			  &rx_k, &rx_k12, &rx_k21, &rx_k13, &rx_k31);
	      }
	    }
	    // Now calculate steady state
	    if (wh0 == 20) {
	      for (int i = ncmt + oral0; i--;){
		A[i] += aSave[i];
	      }
	    }
	    extraAdvan=0;
	  } else if (wh0 == 30) {
	    // Turning off a compartment; Not supported put everything to NaN
	    for (int i = ncmt + oral0; i--;){
	      A[i] = R_NaN;
	    }
	    extraAdvan=0;
	  }
	  // dosing to cmt
	  amt = ind->dose[ind->ixds];
	  switch (whI){
	  case 0: { // Bolus dose
	    // base dose
	    if (cmtOff == 0){
	      b1 = amt*d_F;
	    } else {
	      b2 = amt*d_F2;
	    }
	  } break;
	  case 4: {
	    doReplace = cmtOff+1;
	  } break;
	  case 5: {
	    doMultiply= cmtOff+1;
	  } break;
	  case 9: { // modeled rate.
	    // These are specified in the linCmt
	    if (cmtOff == 0)  {
	      // Infusion to central compartment with oral dosing
	      rateAdjust = d_rate1;
	    } else {
	      // Infusion to central compartment or depot
	      rateAdjust = d_rate2;
	    }
	    // Save rate turn off in next dose
	    doRate = cmtOff+1;
	  } break;
	  case 8: { // modeled duration. 
	    //InfusionRate[cmt] -= dose[ind->ixds+1];
	    if (cmtOff == 0) {
	      // With oral dosing infusion to central compartment
	      rateAdjust = amt/d_dur1*d_F;
	    } else {
	      // Infusion to compartment #1 or depot
	      rateAdjust = amt/d_dur2*d_F2;
	    }
	    doRate = cmtOff+1;
	  } break;
	  case 7:{ // End modeled rate
	    if (cmtOff == 0)  {
	      // Infusion to central compartment with oral dosing
	      rateAdjust = -d_rate1;
	    } else {
	      // Infusion to central compartment or depot
	      rateAdjust = -d_rate2;
	    }
	    doRate = cmtOff+1;
	  } break;
	  case 1: { // Begin infusion
	    rateAdjust = amt; // Amt is negative when turning off
	    doRate = cmtOff+1;
	  } break;
	  case 6: { // end modeled duration
	    if (cmtOff == 0) {
	      // With oral dosing infusion to central compartment
	      rateAdjust = -amt/d_dur1*d_F;
	    } else {
	      // Infusion to compartment #1 or depot
	      rateAdjust = -amt/d_dur2*d_F2;
	    }
	    doRate = cmtOff+1;
	  } break;
	  case 2: {
	    // In this case bio-availability changes the rate, but the duration remains constant.
	    // rate = amt/dur
	    if (cmtOff == 0){
	      rateAdjust = amt*d_F;
	    } else {
	      rateAdjust = amt*d_F2;
	    }
	    doRate = cmtOff+1;
	  } break;
	  }
	  if (wh0 == 40){
	    // Steady state infusion
	    // Now advance to steady state dosing
	    // These are easy to solve
	    if (op->nlinR == 2){
	      rate[0]=0.0;
	      rate[1]=0.0;
	    } else {
	      rate[0] = 0.0;
	    }
	    if (doRate == 1){
	      r1 = rateAdjust;
	      r2 = 0;
	    } else {
	      r1 = 0;
	      r2 = rateAdjust;
	    }
	    doRate=0;
	    ssRate(A, ncmt, oral0, &r1, &r2,
		   &d_ka, &rx_k, &rx_k12, &rx_k21,
		   &rx_k13, &rx_k31);
	    extraAdvan=0;
	  }
	}
      }
      if (extraAdvan){
	doAdvan(A, Alast, tlast, // Time of last amounts
		curTime, ncmt, oral0, &b1, &b2, &r1, &r2,
		&d_ka, &rx_k, &rx_k12, &rx_k21,
		&rx_k13, &rx_k31);
      }
      if (doReplace){
	A[doReplace-1] = amt;
      } else if (doMultiply){
	A[doMultiply-1] *= amt;
      } else if (doRate){
	rate[doRate-1] += rateAdjust;
      }
      rateAdjust=0;
      doRate = doMultiply=doReplace=doInf=0;
      extraAdvan=1;
      for (int i = ncmt + oral0; i--;){
	Alast[i] += A[i];
      }
      tlast = curTime;
    }
  }
  ind->ixds = oldIxds;
  ind->idx = oldIdx;
  return A[oral0]/rx_v;
}

double linCmtD(rx_solve *rx, unsigned int id, double t, int linCmt,
	       int i_cmt, int trans, int val,
	       double p1, double v1,
	       double p2, double p3,
	       double p4, double p5,
	       double d_tlag, double d_F, double d_rate1, double d_dur1,
	       // Oral parameters
	       double d_ka, double d_tlag2, double d_F2,  double d_rate2, double d_dur2) {
  // Forward difference
  double v0 = linCmtA(rx, id, t, linCmt, i_cmt, trans, p1, v1,
		      p2, p3, p4, p5, d_tlag, d_F, d_rate1, d_dur1, 
		      d_ka, d_tlag2, d_F2,  d_rate2, d_dur2);
#define h 1.4901161193847656e-08
#define h2 67108864
  switch (val){
  case 0:
    return v0;
  case 1:
    return h2*(linCmtC(rx, id, t, linCmt, i_cmt, trans, p1 + h, v1,
		      p2, p3, p4, p5, d_tlag, d_F, d_rate1, d_dur1, 
		       d_ka, d_tlag2, d_F2,  d_rate2, d_dur2)- v0);
  case 2:
    return h2*(linCmtC(rx, id, t, linCmt, i_cmt, trans, p1, v1 + h,
		       p2, p3, p4, p5, d_tlag, d_F, d_rate1, d_dur1, 
		       d_ka, d_tlag2, d_F2,  d_rate2, d_dur2) - v0);
  case 3:
    return h2*(linCmtC(rx, id, t, linCmt, i_cmt, trans, p1, v1,
		      p2 + h, p3, p4, p5, d_tlag, d_F, d_rate1, d_dur1, 
		       d_ka, d_tlag2, d_F2,  d_rate2, d_dur2) - v0);
  case 4:
    return h2*(linCmtC(rx, id, t, linCmt, i_cmt, trans, p1, v1,
		      p2, p3 + h, p4, p5, d_tlag, d_F, d_rate1, d_dur1, 
		      d_ka, d_tlag2, d_F2,  d_rate2, d_dur2) - v0);
  case 5:
    return h2*(linCmtC(rx, id, t, linCmt, i_cmt, trans, p1, v1,
		      p2, p3, p4 + h, p5, d_tlag, d_F, d_rate1, d_dur1, 
		       d_ka, d_tlag2, d_F2,  d_rate2, d_dur2) - v0);
  case 6:
    return h2*(linCmtC(rx, id, t, linCmt, i_cmt, trans, p1, v1,
		       p2, p3, p4, p5 + h, d_tlag, d_F, d_rate1, d_dur1, 
		       d_ka, d_tlag2, d_F2,  d_rate2, d_dur2) - v0);
  case 7:
    return h2*(linCmtC(rx, id, t, linCmt, i_cmt, trans, p1, v1,
		       p2, p3, p4, p5, d_tlag + h, d_F, d_rate1, d_dur1, 
		       d_ka, d_tlag2, d_F2,  d_rate2, d_dur2) - v0);
  case 8:
    return h2*(linCmtC(rx, id, t, linCmt, i_cmt, trans, p1, v1,
		       p2, p3, p4, p5, d_tlag, d_F + h, d_rate1, d_dur1, 
		       d_ka, d_tlag2, d_F2,  d_rate2, d_dur2) - v0);
  case 9:
    return h2*(linCmtC(rx, id, t, linCmt, i_cmt, trans, p1, v1,
		       p2, p3, p4, p5, d_tlag, d_F, d_rate1 + h, d_dur1, 
		       d_ka, d_tlag2, d_F2,  d_rate2, d_dur2) - v0);
  case 10:
    return h2*(linCmtC(rx, id, t, linCmt, i_cmt, trans, p1, v1,
		       p2, p3, p4, p5, d_tlag, d_F, d_rate1, d_dur1 + h, 
		       d_ka, d_tlag2, d_F2,  d_rate2, d_dur2) - v0);
  case 11:
    return h2*(linCmtC(rx, id, t, linCmt, i_cmt, trans, p1, v1,
		      p2, p3, p4, p5, d_tlag, d_F, d_rate1, d_dur1,
		       d_ka + h, d_tlag2, d_F2,  d_rate2, d_dur2) - v0);
  case 12:
    return h2*(linCmtC(rx, id, t, linCmt, i_cmt, trans, p1, v1,
		       p2, p3, p4, p5, d_tlag, d_F, d_rate1, d_dur1,
		       d_ka, d_tlag2 + h, d_F2,  d_rate2, d_dur2) - v0);
  case 13:
    return h2*(linCmtC(rx, id, t, linCmt, i_cmt, trans, p1, v1,
		       p2, p3, p4, p5, d_tlag, d_F, d_rate1, d_dur1,
		       d_ka, d_tlag2, d_F2  + h,  d_rate2, d_dur2) - v0);
  case 14:
    return h2*(linCmtC(rx, id, t, linCmt, i_cmt, trans, p1, v1,
		       p2, p3, p4, p5, d_tlag, d_F, d_rate1, d_dur1,
		       d_ka, d_tlag2, d_F2,  d_rate2 + h, d_dur2) - v0);
  case 15:
    return h2*(linCmtC(rx, id, t, linCmt, i_cmt, trans, p1, v1,
		       p2, p3, p4, p5, d_tlag, d_F, d_rate1, d_dur1,
		       d_ka, d_tlag2, d_F2,  d_rate2, d_dur2 + h) - v0);
  default:
    error("undef diff");
  }
#undef h
#undef h2
}

double linCmtE(rx_solve *rx, unsigned int id, double t, int linCmt,
	       int i_cmt, int trans, int val,
	       double p1, double v1,
	       double p2, double p3,
	       double p4, double p5,
	       double d_tlag, double d_F, double d_rate1, double d_dur1,
	       // Oral parameters
	       double d_ka, double d_tlag2, double d_F2,  double d_rate2, double d_dur2) {
  // Central difference
#define h 7.4505805969238281e-09
#define h2 134217728
  switch (val){
  case 0:
    return linCmtA(rx, id, t, linCmt, i_cmt, trans, p1, v1,
		      p2, p3, p4, p5, d_tlag, d_F, d_rate1, d_dur1, 
		      d_ka, d_tlag2, d_F2,  d_rate2, d_dur2);
  case 1:
    return h2*(linCmtC(rx, id, t, linCmt, i_cmt, trans, p1 + h, v1,
		       p2, p3, p4, p5, d_tlag, d_F, d_rate1, d_dur1, 
		       d_ka, d_tlag2, d_F2,  d_rate2, d_dur2) -
	       linCmtC(rx, id, t, linCmt, i_cmt, trans, p1 - h, v1,
		       p2, p3, p4, p5, d_tlag, d_F, d_rate1, d_dur1, 
		       d_ka, d_tlag2, d_F2,  d_rate2, d_dur2));
  case 2:
    return h2*(linCmtC(rx, id, t, linCmt, i_cmt, trans, p1, v1 + h,
		       p2, p3, p4, p5, d_tlag, d_F, d_rate1, d_dur1, 
		       d_ka, d_tlag2, d_F2,  d_rate2, d_dur2) -
	       linCmtC(rx, id, t, linCmt, i_cmt, trans, p1, v1 - h,
		       p2, p3, p4, p5, d_tlag, d_F, d_rate1, d_dur1, 
		       d_ka, d_tlag2, d_F2,  d_rate2, d_dur2));
  case 3:
    return h2*(linCmtC(rx, id, t, linCmt, i_cmt, trans, p1, v1,
		       p2 + h, p3, p4, p5, d_tlag, d_F, d_rate1, d_dur1, 
		       d_ka, d_tlag2, d_F2,  d_rate2, d_dur2) -
	       linCmtC(rx, id, t, linCmt, i_cmt, trans, p1, v1,
		       p2 - h, p3, p4, p5, d_tlag, d_F, d_rate1, d_dur1, 
		       d_ka, d_tlag2, d_F2,  d_rate2, d_dur2));
  case 4:
    return h2*(linCmtC(rx, id, t, linCmt, i_cmt, trans, p1, v1,
		       p2, p3 + h, p4, p5, d_tlag, d_F, d_rate1, d_dur1, 
		       d_ka, d_tlag2, d_F2,  d_rate2, d_dur2) -
	       linCmtC(rx, id, t, linCmt, i_cmt, trans, p1, v1,
		       p2, p3 - h, p4, p5, d_tlag, d_F, d_rate1, d_dur1, 
		       d_ka, d_tlag2, d_F2,  d_rate2, d_dur2));
  case 5:
    return h2*(linCmtC(rx, id, t, linCmt, i_cmt, trans, p1, v1,
		       p2, p3, p4 + h, p5, d_tlag, d_F, d_rate1, d_dur1, 
		       d_ka, d_tlag2, d_F2,  d_rate2, d_dur2) -
	       linCmtC(rx, id, t, linCmt, i_cmt, trans, p1, v1,
		       p2, p3, p4 - h, p5, d_tlag, d_F, d_rate1, d_dur1, 
		       d_ka, d_tlag2, d_F2,  d_rate2, d_dur2));
  case 6:
    return h2*(linCmtC(rx, id, t, linCmt, i_cmt, trans, p1, v1,
		       p2, p3, p4, p5 + h, d_tlag, d_F, d_rate1, d_dur1, 
		       d_ka, d_tlag2, d_F2,  d_rate2, d_dur2) -
	       linCmtC(rx, id, t, linCmt, i_cmt, trans, p1, v1,
			  p2, p3, p4, p5 - h, d_tlag, d_F, d_rate1, d_dur1, 
			  d_ka, d_tlag2, d_F2,  d_rate2, d_dur2));
  case 7:
    return h2*(linCmtC(rx, id, t, linCmt, i_cmt, trans, p1, v1,
		       p2, p3, p4, p5, d_tlag + h, d_F, d_rate1, d_dur1, 
		       d_ka, d_tlag2, d_F2,  d_rate2, d_dur2) -
	       linCmtC(rx, id, t, linCmt, i_cmt, trans, p1, v1,
		       p2, p3, p4, p5, d_tlag - h, d_F, d_rate1, d_dur1, 
		       d_ka, d_tlag2, d_F2,  d_rate2, d_dur2));
  case 8:
    return h2*(linCmtC(rx, id, t, linCmt, i_cmt, trans, p1, v1,
		       p2, p3, p4, p5, d_tlag, d_F + h, d_rate1, d_dur1, 
		       d_ka, d_tlag2, d_F2,  d_rate2, d_dur2) -
	       linCmtC(rx, id, t, linCmt, i_cmt, trans, p1, v1,
		       p2, p3, p4, p5, d_tlag, d_F - h, d_rate1, d_dur1, 
		       d_ka, d_tlag2, d_F2,  d_rate2, d_dur2));
  case 9:
    return h2*(linCmtC(rx, id, t, linCmt, i_cmt, trans, p1, v1,
		       p2, p3, p4, p5, d_tlag, d_F, d_rate1 + h, d_dur1, 
		       d_ka, d_tlag2, d_F2,  d_rate2, d_dur2) -
	       linCmtC(rx, id, t, linCmt, i_cmt, trans, p1, v1,
		       p2, p3, p4, p5, d_tlag, d_F, d_rate1 - h, d_dur1, 
		       d_ka, d_tlag2, d_F2,  d_rate2, d_dur2));
  case 10:
    return h2*(linCmtC(rx, id, t, linCmt, i_cmt, trans, p1, v1,
		       p2, p3, p4, p5, d_tlag, d_F, d_rate1, d_dur1 + h, 
		       d_ka, d_tlag2, d_F2,  d_rate2, d_dur2) -
	       linCmtC(rx, id, t, linCmt, i_cmt, trans, p1, v1,
		       p2, p3, p4, p5, d_tlag, d_F, d_rate1, d_dur1 - h, 
		       d_ka, d_tlag2, d_F2,  d_rate2, d_dur2));
  case 11:
    return h2*(linCmtC(rx, id, t, linCmt, i_cmt, trans, p1, v1,
		       p2, p3, p4, p5, d_tlag, d_F, d_rate1, d_dur1,
		       d_ka + h, d_tlag2, d_F2,  d_rate2, d_dur2) -
	       linCmtC(rx, id, t, linCmt, i_cmt, trans, p1, v1,
		       p2, p3, p4, p5, d_tlag, d_F, d_rate1, d_dur1,
		       d_ka - h, d_tlag2, d_F2,  d_rate2, d_dur2));
  case 12:
    return h2*(linCmtC(rx, id, t, linCmt, i_cmt, trans, p1, v1,
		       p2, p3, p4, p5, d_tlag, d_F, d_rate1, d_dur1,
		       d_ka, d_tlag2 + h, d_F2,  d_rate2, d_dur2) -
	       linCmtC(rx, id, t, linCmt, i_cmt, trans, p1, v1,
		       p2, p3, p4, p5, d_tlag, d_F, d_rate1, d_dur1,
		       d_ka, d_tlag2 - h, d_F2,  d_rate2, d_dur2));
  case 13:
    return h2*(linCmtC(rx, id, t, linCmt, i_cmt, trans, p1, v1,
		       p2, p3, p4, p5, d_tlag, d_F, d_rate1, d_dur1,
		       d_ka, d_tlag2, d_F2  + h,  d_rate2, d_dur2) +
	       linCmtC(rx, id, t, linCmt, i_cmt, trans, p1, v1,
		       p2, p3, p4, p5, d_tlag, d_F, d_rate1, d_dur1,
		       d_ka, d_tlag2, d_F2 - h,  d_rate2, d_dur2));
  case 14:
    return h2*(linCmtC(rx, id, t, linCmt, i_cmt, trans, p1, v1,
		       p2, p3, p4, p5, d_tlag, d_F, d_rate1, d_dur1,
		       d_ka, d_tlag2, d_F2,  d_rate2 + h, d_dur2) +
	       linCmtC(rx, id, t, linCmt, i_cmt, trans, p1, v1,
		       p2, p3, p4, p5, d_tlag, d_F, d_rate1, d_dur1,
		       d_ka, d_tlag2, d_F2,  d_rate2 - h, d_dur2));
  case 15:
    return h2*(linCmtC(rx, id, t, linCmt, i_cmt, trans, p1, v1,
		       p2, p3, p4, p5, d_tlag, d_F, d_rate1, d_dur1,
		       d_ka, d_tlag2, d_F2,  d_rate2, d_dur2 + h) -
	       linCmtC(rx, id, t, linCmt, i_cmt, trans, p1, v1,
		       p2, p3, p4, p5, d_tlag, d_F, d_rate1, d_dur1,
		       d_ka, d_tlag2, d_F2,  d_rate2, d_dur2 - h));
  default:
    error("undef diff");
  }
#undef h
#undef h2
}

extern double linCmtBB(rx_solve *rx, unsigned int id,
		       double t, int linCmt,
		       int ncmt, int trans, int val,
		       double dd_p1, double dd_v1,
		       double dd_p2, double dd_p3,
		       double dd_p4, double dd_p5,
		       double dd_tlag, double dd_F,
		       double dd_rate, double dd_dur,
		       // oral extra parameters
		       double dd_ka, double dd_tlag2,
		       double dd_F2, double dd_rate2, double dd_dur2);

static inline void ssRateTauD(double *A,
			      int ncmt,
			      int oral0,
			      double *tinf,
			      double *tau,
			      double *r1,
			      double *r2,
			      double *ka, 
			      double *kel,  
			      double *k12, double *k21){
  if (oral0){
    if ((*r1) > 0 ){
      switch (ncmt){
      case 1: {
	oneCmtKaRateSStr1(A, tinf, tau, r1, ka, kel);
	return;
      } break;
      case 2: {
	twoCmtKaRateSStr1D(A, tinf, tau, r1, ka, kel, k12, k21);
	return;
      } break;
      }
    } else {
      switch (ncmt){
      case 1: {
	oneCmtKaRateSStr2D(A, tinf, tau, r2, ka, kel);
	return;
      } break;
      case 2: {
	twoCmtKaRateSStr2D(A, tinf, tau, r2, ka, kel, k12, k21);
	return;
      } break;
      }
    }
  } else {
    switch (ncmt){
    case 1: {
      oneCmtRateSSD(A, tinf, tau, r1, kel);
      return;
    } break;
    case 2: {
      twoCmtRateSSD(A, tinf, tau, r1, kel, k12, k21);
      return;
    } break;
    }
  }
}

static inline void ssTauD(double *A,
			  int ncmt,
			  int oral0,
			  double *tau,
			  double *b1,
			  double *b2,
			  double *ka, // ka (for oral doses)
			  double *kel,  //double rx_v,
			  double *k12, double *k21){
  if (oral0){
    if ((*b1) > 0 ){
      switch (ncmt){
      case 1: {
	oneCmtKaSSb1D(A, tau, b1, ka, kel);
	return;
      } break;
      case 2: {
	twoCmtKaSSb1D(A, tau, b1, ka, kel, k12, k21);
	return;
      } break;
      }
    } else {
      switch (ncmt){
      case 1: {
	oneCmtKaSSb2D(A, tau, b2, ka, kel);
	return;
      } break;
      case 2: {
	twoCmtKaSSb2D(A, tau, b2, ka, kel, k12, k21);
	return;
      } break;
      }
    }
  } else {
    switch (ncmt){
    case 1: {
      oneCmtBolusSSD(A, tau, b1, kel);
      return;
    } break;
    case 2: {
      twoCmtBolusSSD(A, tau, b1, kel, k12, k21);
      return;
    } break;
    }
  }
}

static inline void ssRateD(double *A,
			  int ncmt, // Number of compartments
			  int oral0, // Indicator of if this is an oral system)
			  double *r1, // Rate in Compartment #1
			  double *r2, // Rate in Compartment #2
			  double *ka, // ka (for oral doses)
			  double *kel,  //double rx_v,
			  double *k12, double *k21) {
  if (oral0){
    if ((*r1) > 0){
      switch (ncmt){
      case 1: {
	oneCmtKaRateSSr1D(A, r1, ka, kel);
	return;
      } break;
      case 2: {
	twoCmtKaRateSSr1D(A, r1, ka, kel, k12, k21);
	return;
      } break;
      }
    } else {
      switch (ncmt){
      case 1: {
	oneCmtKaRateSSr2D(A, r2, ka, kel);
	return;
      } break;
      case 2: {
	twoCmtKaRateSSr2D(A, r2, ka, kel, k12, k21);
	return;
      } break;
      }
    }
  } else {
    switch (ncmt){
    case 1: {
      oneCmtRateSSr1D(A, r1, kel);
      return;
    } break;
    case 2: {
      twoCmtRateSSr1D(A, r1, kel, k12, k21);
      return;
    } break;
    }
  }
}

static inline void doAdvanD(double *A,// Amounts
			    double *Alast, // Last amounts
			    double tlast, // Time of last amounts
			    double ct, // Time of the dose
			    int ncmt, // Number of compartments
			    int oral0, // Indicator of if this is an oral system
			    double *b1, // Amount of the dose in compartment #1
			    double *b2, // Amount of the dose in compartment #2
			    double *r1, // Rate in Compartment #1
			    double *r2, // Rate in Compartment #2
			    double *ka, // ka (for oral doses)
			    double *kel,  //double rx_v,
			    double *k12, double *k21){
  double t = ct - tlast;
    if (oral0){
      switch (ncmt){
      case 1: {
	oneCmtKaRateD(A, Alast, &t, b1, b2, r1, r2, ka, kel);
	return;
      } break;
      case 2: {
	twoCmtKaRateD(A, Alast, &t, b1, b2, r1, r2,
		      ka,  kel, k12, k21);
	return;
      } break;
      }
    } else {
      switch (ncmt){
      case 1: {
	oneCmtRateD(A, Alast, &t, b1, r1, kel);
	return;
      } break;
      case 2: {
	twoCmtRateD(A, Alast, &t, b1, r1,
		    kel, k12, k21);
	return;
      } break;
      }
    }
}

double derTrans(double *A, int ncmt, int trans, int val,
		double p1, double v1,
		double p2, double p3,
		double p4, double p5,
		double d_tlag, double d_F, double d_rate1, double d_dur1,
		// Oral parameters
		double d_ka, double d_tlag2, double d_F2,  double d_rate2, double d_dur2) {
  int oral0 = (d_ka > 0) ? 1 : 0;
  if (val == 0){
    // Apply trans
    if (trans == 10) {
      return(A[oral0]*(v1+p3+p5));
    } else {
      return(A[oral0]/v1);
    }
  }
  switch (ncmt) {
  case 2: // 2 compartment model
    if (val == 11) {
      // d/dt(ka)
      if (trans == 10) {
	return A[6]*v1;
      } else {
	return A[6]/v1;
      }
    }
    switch (trans){
    case 1: // cl=(*p1) v=(*v1) q=(*p2) vp=(*p3)
      switch (val) {
      case 1: // cl
	return A[2 + oral0 * 5]/(v1 * v1);
      case 2: // v
	return -A[oral0]/(v1 * v1) +
	  (-p1 * A[2 + oral0 * 5]/(v1 * v1) -
	   p2 * A[3 + oral0 * 5]/(v1 *v1))/v1;
      case 3: // q
	return (A[4 + oral0 * 5]/p3 + A[3 + oral0 * 5]/v1)/v1;
      case 4: // vp
	return -p2 * A[4 + oral0 * 5]/(p3 * p3 * v1);
      }
    case 2: // Direct translation
      switch (val) {
      case 1: // k
	// A1k10 A[2]
	// A2k20 A[7]
	return A[2+oral0*5]/v1;
      case 2: // v
	return -A[oral0]/(v1*v1);
      case 3: // k12
	return A[3+oral0*5]/v1;
      case 4: // k21
	return A[4+oral0*5]/v1;
      }
    case 3: // cl=(*p1) v=(*v1) q=(*p2) vss=(*p3)
      switch (val) {
      case 1: // cl
	return A[2 + oral0 * 5]/(v1 * v1);
      case 2: // v
	return -A[oral0]/(v1 * v1) + (-p1 * A[2 + oral0 * 5]/(v1 * v1) -
				      p2 * A[3 + oral0 * 5]/(v1 * v1) +
				      p2 * A[4 + oral0 * 5]/((p3 - v1) * (p3 - v1)))/v1;
      case 3: // q
	return (A[3 + oral0 * 5]/v1 + A[4 + oral0 * 5]/(p3 - v1))/v1;
      case 4: // vss
	return -p2 * A[4 + oral0 * 5]/((p3 - v1) * (p3 - v1) * v1);
      }
    case 4: // alpha=(*p1) beta=(*p2) k21=(*p3)
      switch (val) {
      case 1: // alpha
	return ((1 - p2/p3) * A[3 + oral0 * 5] + p2 * A[2 + oral0 * 5]/p3)/v1;
      case 2: // v
	return -A[oral0]/(v1 * v1);
      case 3: // beta
	return ((1 - p1/p3) * A[3 + oral0 * 5] + p1 * A[2 + oral0 * 5]/p3)/v1;
      case 4: // k21
	return (A[3 + oral0 * 5] * (-1 + p2 * p1/(p3 * p3)) -
		p2 * p1 * A[2 + oral0 * 5]/(p3 * p3) +
		A[4 + oral0 * 5])/v1;
      }
    case 5: // alpha=(*p1) beta=(*p2) aob=(*p3)
      switch (val) {
      case 1: // alpha
	return (A[4 + oral0 * 5]/(1 + p3) + (p2 * (1 + p3)/(p1 + p2 * p3) -
					     p2 * p1 * (1 + p3)/((p1 + p2 * p3) * (p1 + p2 * p3))) * A[2 + oral0 * 5] +
		A[3 + oral0 * 5] * (1 - p2 * (1 + p3)/(p1 + p2 * p3) +
				    p2 * p1 * (1 + p3)/((p1 + p2 * p3) * (p1 + p2 * p3)) -
				    (1/(1 + p3))))/v1;
      case 2: // v
	return -A[oral0]/(v1 * v1);
      case 3: // beta
	return ((p1 * (1 + p3)/(p1 + p2 * p3) -
		 p2 * p1 * p3 * (1 + p3)/((p1 + p2 * p3) * (p1 + p2 * p3))) * A[2 + oral0 * 5] +
		(1 - p3/(1 + p3) - p1 * (1 + p3)/(p1 + p2 * p3) +
		 p2 * p1 * p3 * (1 + p3)/((p1 + p2 * p3) * (p1 + p2 * p3))) * A[3 + oral0 * 5] +
		p3 * A[4 + oral0 * 5]/(1 + p3))/v1;
      case 4: // aob
	return ((p2/(1 + p3) - (p1 + p2 * p3)/((1 + p3) * (1 + p3))) * A[4 + oral0 * 5] +
		A[2 + oral0 * 5] * (p2 * p1/(p1 + p2 * p3) -
				    (p2*p2) * p1 * (1 + p3)/((p1 + p2 * p3) * (p1 + p2 * p3))) +
		A[3 + oral0 * 5] * (-p2/(1 + p3) + (p1 + p2 * p3)/((1 + p3) * (1 + p3)) -
				    p2 * p1/(p1 + p2 * p3) +
				    (p2 * p2) * p1 * (1 + p3)/((p1 + p2 * p3) * (p1 + p2 * p3))))/v1;
      }
    case 11: // A2 V, alpha=(*p1), beta=(*p2), k21
      switch (val) {
      case 1: // alpha
	return (A[2 + oral0 * 5] * ( p2 * (p3 +  1.0/v1)/(p1 * p3 +  p2/v1) -  p2 * p1 * p3 * (p3 +  1.0/v1)/(((p1 * p3 +  p2/v1)) * ((p1 * p3 +  p2/v1)))) + A[3 + oral0 * 5] * (1 -  p3/(p3 +  1.0/v1) -  p2 * (p3 +  1.0/v1)/(p1 * p3 +  p2/v1) +  p2 * p1 * p3 * (p3 +  1.0/v1)/(((p1 * p3 +  p2/v1)) * ((p1 * p3 +  p2/v1)))) +  p3 * A[4 + oral0 * 5]/(p3 +  1.0/v1))/v1;
      case 2: // v
	return -A[oral0]/(v1*v1) + (A[2 + oral0 * 5] * (- p2 * p1/((v1*v1) * (p1 * p3 +  p2/v1)) +  (p2*p2) * p1 * (p3 +  1.0/v1)/((v1*v1) * (((p1 * p3 +  p2/v1)) * ((p1 * p3 +  p2/v1))))) + A[3 + oral0 * 5] * (  p2/((v1*v1) * (p3 +  1.0/v1)) -  (p1 * p3 +  p2/v1)/((v1*v1) * (((p3 +  1.0/v1)) * ((p3 +  1.0/v1)))) +  p2 * p1/((v1*v1) * (p1 * p3 +  p2/v1)) -  (p2*p2) * p1 * (p3 +  1.0/v1)/((v1*v1) *      (((p1 * p3 +  p2/v1)) * ((p1 * p3 +  p2/v1))))) + A[4 + oral0 * 5] * (- p2/((v1*v1) * (p3 +  1.0/v1)) +  (p1 * p3 +  p2/v1)/((v1*v1) * (((p3 +  1.0/v1)) * ((p3 +  1.0/v1))))))/v1;
      case 3: // beta
	return (A[2 + oral0 * 5] * ( p1 * (p3 +  1.0/v1)/(p1 * p3 +  p2/v1) -  p2 * p1 * (p3 +  1.0/v1)/(v1 * (((p1 * p3 +  p2/v1)) * ((p1 * p3 +  p2/v1))))) + A[3 + oral0 * 5] * (1 -  1/(v1 * (p3 +  1.0/v1)) -  p1 * (p3 +  1.0/v1)/(p1 * p3 +  p2/v1) +  p2 * p1 * (p3 +  1.0/v1)/(v1 * (((p1 * p3 +  p2/v1)) * ((p1 * p3 +  p2/v1))))) +  A[4 + oral0 * 5]/(v1 * (p3 +  1.0/v1)))/v1;
      case 4: // k21
	return ((0 -  p1/(p3 +  1.0/v1) +  (p1 * p3 +  p2/v1)/(((p3 +  1.0/v1)) * ((p3 +  1.0/v1))) -  p2 * p1/(p1 * p3 +  p2/v1) +  p2 * ((p1) * (p1)) * (p3 +  1.0/v1)/(((p1 * p3 +  p2/v1)) * ((p1 * p3 +  p2/v1)))) * A[3 + oral0 * 5] + A[2 + oral0 * 5] * (p2 * p1/(p1 * p3 +  p2/v1) -  p2 * ((p1) * (p1)) * (p3 +  1.0/v1)/(((p1 * p3 +  p2/v1)) * ((p1 * p3 +  p2/v1)))) + A[4 + oral0 * 5] * ( p1/(p3 + 1.0/v1) -  (p1 * p3 +  p2/v1)/((p3 + 1.0/v1) * (p3 +  1.0/v1))))/v1;
      }
    case 10: // A=(*v1), alpha=(*p1), beta=(*p2), B=(*p3)
      switch (val) {
      case 1: // alpha
	return (( (p3 + v1) * p2/(p1 * p3 + p2 * v1) -  (p3 + v1) * p2 * p1 * p3/(((p1 * p3 + p2 * v1)) * ((p1 * p3 + p2 * v1)))) * A[2 + oral0 * 5] + A[3 + oral0 * 5] * (1 -  p3/(p3 + v1) -  (p3 + v1) * p2/(p1 * p3 + p2 * v1) +  (p3 + v1) * p2 * p1 * p3/(((p1 * p3 + p2 * v1)) * ((p1 * p3 + p2 * v1)))) +  p3 * A[4 + oral0 * 5]/(p3 + v1))/v1;
      case 2: // A
	return -A[oral0]/(v1*v1) + (( p2 * p1/(p1 * p3 + p2 * v1) -  (p3 + v1) * (p2*p2) * p1/(((p1 * p3 + p2 * v1)) * ((p1 * p3 + p2 * v1)))) * A[2 + oral0 * 5] + A[3 + oral0 * 5] * (0 -  p2/(p3 + v1) +  (p1 * p3 + p2 * v1)/(((p3 + v1)) * ((p3 + v1))) -  p2 * p1/(p1 * p3 + p2 * v1) +  (p3 + v1) * (p2*p2) * p1/(((p1 * p3 + p2 * v1)) * ((p1 * p3 + p2 * v1)))) + A[4 + oral0 * 5] * ( p2/(p3 + v1) -  (p1 * p3 + p2 * v1)/(((p3 + v1)) * ((p3 + v1)))))/v1;
      case 3: // beta
	return (( (p3 + v1) * p1/(p1 * p3 + p2 * v1) -  (p3 + v1) * p2 * p1 * v1/(((p1 * p3 + p2 * v1)) * ((p1 * p3 + p2 * v1)))) * A[2 + oral0 * 5] + A[3 + oral0 * 5] * (1 -  v1/(p3 + v1) -  (p3 + v1) * p1/(p1 * p3 + p2 * v1) +  (p3 + v1) * p2 * p1 * v1/(((p1 * p3 + p2 * v1)) * ((p1 * p3 + p2 * v1)))) +  v1 * A[4 + oral0 * 5]/(p3 + v1))/v1;
      case 4: // B
	return (( p2 * p1/(p1 * p3 + p2 * v1) -  (p3 + v1) * p2 * ((p1) * (p1))/(((p1 * p3 + p2 * v1)) * ((p1 * p3 + p2 * v1)))) * A[2 + oral0 * 5] + A[3 + oral0 * 5] * (0 -  p1/(p3 + v1) +  (p1 * p3 + p2 * v1)/(((p3 + v1)) * ((p3 + v1))) -  p2 * p1/(p1 * p3 + p2 * v1) +  (p3 + v1) * p2 * ((p1) * (p1))/(((p1 * p3 + p2 * v1)) * ((p1 * p3 + p2 * v1)))) + A[4 + oral0 * 5] * ( p1/(p3 + v1) -  (p1 * p3 + p2 * v1)/(((p3 + v1)) * ((p3 + v1)))))/v1;
      }
    }
  case 1: // One compartment model
    if (val == 11) {
      // d/dt(ka)
      if (trans == 10) {
	return A[3]*v1;
      } else {
	return A[3]/v1;
      }
    }
    switch (trans){
    case 1: // cl v
      switch (val){
      case 1: // d/dt(p1) = d/dt(cl) f = A[oral0]/v
	//A2k20 = A[4]
	//A1k10 = A[1]
	return A[oral0*3 + 1]/(v1*v1);
      case 2: // d/dt(v)
	return -A[oral0]/(v1*v1) - p1*A[oral0*3+1]/(v1*v1*v1);
      }
    case 2: // k V
      switch (val) {
      case 1: // d/dt(p1) = d/dt(kel)
	//A2k20 = A[4]
	//A1k10 = A[1]
	return A[oral0*3 + 1]/(v1);
      case 2: // d/dt(v)
	return -A[oral0]/(v1*v1);
      }
    case 11: // alpha V
      switch (val) {
      case 1: // d/dt(p1) = d/dt(alpha)  //alpha=kel
	return A[oral0*3 + 1]/(v1);
      case 2: // d/dt(v)
	return -A[oral0]/(v1*v1);
      }
    case 10: // alpha A
      switch (val) {
      case 1: // d/dt(alpha)
	return A[oral0*3+1]*v1;
      case 2: // d/dt(A)
	return A[oral0];
      }
    } break;
  }
  return R_NaN;
}

double linCmtF(rx_solve *rx, unsigned int id, double t, int linCmt,
	       int ncmt, int trans, int val,
	       double p1, double v1,
	       double p2, double p3,
	       double p4, double p5,
	       double d_tlag, double d_F, double d_rate1, double d_dur1,
	       // Oral parameters
	       double d_ka, double d_tlag2, double d_F2,  double d_rate2, double d_dur2) {
  if (ncmt == 3) { // for now use autodiff
    return linCmtBB(rx, id, t, linCmt, ncmt, trans, val,
		    p1, v1, p2, p3,
		    p4, p5, d_tlag, d_F,
		    d_rate1, d_dur1, d_ka, d_tlag2, d_F2,
		    d_rate2, d_dur2);
  } else {

    /* REprintf("F: %f; F2: %f\n", d_F, d_F2); */
    rx_solving_options_ind *ind = &(rx->subjects[id]);
    int evid, wh, cmt, wh100, whI, wh0;
    /* evid = ind->evid[ind->ix[ind->idx]]; */
    /* if (evid) REprintf("evid0[%d:%d]: %d; curTime: %f\n", id, ind->idx, evid, t); */
    int idx = ind->idx;
    double Alast0[13] = {0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0};
    sortIfNeeded(rx, ind, id, &linCmt, &d_tlag, &d_tlag2, &d_F, &d_F2,
		 &d_rate1, &d_dur1, &d_rate2, &d_dur2);
    rx_solving_options *op = rx->op;
    int oral0;
    oral0 = (d_ka > 0) ? 1 : 0;
    double *A;
    double *Alast;
    /* A = Alast0; Alast=Alast0; */
    double tlast;
    double it = getTime(ind->ix[idx], ind);
    double curTime=0.0;
    if (t != it) {
      // Try to get another idx by bisection
      /* REprintf("it pre: %f", it); */
      idx = _locateTimeIndex(t, ind);
      it = getTime(ind->ix[idx], ind);
      /* REprintf("it post: %f", it); */
    }
    /* REprintf("idx: %d; solved: %d; t: %f fabs: %f\n", idx, ind->solved[idx], t, fabs(t-it)); */
    int sameTime = fabs(t-it) < sqrt(DOUBLE_EPS);
    if (idx <= ind->solved && sameTime){
      // Pull from last solved value (cached)
      A = ind->linCmtAdvan+(op->nlin)*idx;
      return derTrans(A, ncmt, trans, val, p1, v1, p2, p3,
		      p4, p5, d_tlag,  d_F,  d_rate1,  d_dur1,
		      d_ka, d_tlag2, d_F2, d_rate2, d_dur2);
    }
    unsigned int ncmt = 1;
    double rx_k=0, rx_v=0;
    double rx_k12=0;
    double rx_k21=0;
    double rx_k13=0;
    double rx_k31=0;
    double *rate = ind->linCmtRate;
    double b1=0, b2=0, r1 = 0, r2 = 0;
    A = Alast0;
    if (idx <= ind->solved){
      if (!parTrans(&trans, &p1, &v1, &p2, &p3, &p4, &p5,
		    &ncmt, &rx_k, &rx_v, &rx_k12,
		    &rx_k21, &rx_k13, &rx_k31)){
	REprintf(_("invalid translation\n"));
	return NA_REAL;
      }
    } else {
      A = ind->linCmtAdvan+(op->nlin)*idx;
      if (idx == 0) {
	Alast = Alast0;
	tlast = getTime(ind->ix[0], ind);
      } else {
	tlast = getTime(ind->ix[idx-1], ind);
	Alast = ind->linCmtAdvan+(op->nlin)*(idx-1);
      }
      curTime = getTime(ind->ix[idx], ind);
      if (!parTrans(&trans, &p1, &v1, &p2, &p3, &p4, &p5,
		    &ncmt, &rx_k, &rx_v, &rx_k12,
		    &rx_k21, &rx_k13, &rx_k31)){
	REprintf(_("invalid translation\n"));
	return NA_REAL;
      }
      evid = ind->evid[ind->ix[idx]];
      /* if (evid) REprintf("evid: %d; curTime: %f\n",evid, curTime); */
      if (op->nlinR == 2){
	r1 = rate[0];
	r2 = rate[1];
      } else {
	r1 = rate[0];
      }
      int doMultiply = 0, doReplace=0;
      double amt=0;
      double rateAdjust= 0;
      int doRate=0;
      int extraAdvan = 1;
      wh0 = 0;
      if (isObs(evid)){
	// Only apply doses when you need to set the solved system.
	// When it is an observation of course you do not need to apply the doses
      } else if (evid == 3){
	// Reset event
	Alast=Alast0;
      } else {
	getWh(evid, &wh, &cmt, &wh100, &whI, &wh0);
	/* REprintf("evid: %d; cmt: %d; %d\n", evid, cmt, linCmt); */
	if (!oral0 && cmt > linCmt) {
	  int foundBad = 0;
	  for (int j = 0; j < ind->nBadDose; j++){
	    if (ind->BadDose[j] == cmt+1){
	      foundBad=1;
	      break;
	    }
	  }
	  if (!foundBad){
	    ind->BadDose[ind->nBadDose]=cmt+1;
	    ind->nBadDose++;
	  }
	} else if (oral0 && cmt > linCmt+1) {
	  int foundBad = 0;
	  for (int j = 0; j < ind->nBadDose; j++){
	    if (ind->BadDose[j] == cmt+1){
	      foundBad=1;
	      break;
	    }
	  }
	  if (!foundBad){
	    ind->BadDose[ind->nBadDose]=cmt+1;
	    ind->nBadDose++;
	  }
	}
	int cmtOff = cmt-linCmt;
	if ((oral0 && cmtOff > 1) ||
	    (!oral0 && cmtOff != 0)) {
	} else {
	  syncIdx(ind);
	  amt = ind->dose[ind->ixds];
	  if (!ISNA(amt) && (amt > 0) && (wh0 == 10 || wh0 == 20)) {
	    // dosing to cmt
	    // Steady state doses; wh0 == 20 is equivalent to SS=2 in NONMEM
	    double tau = ind->ii[ind->ixds];
	    // For now advance based solving to steady state (just like ODE)
	    double aSave[15];
	    if (wh0 == 20) {
	      doAdvanD(A, Alast, tlast, // Time of last amounts
		       curTime, ncmt, oral0, &b1, &b2, &r1, &r2,
		       &d_ka, &rx_k, &rx_k12, &rx_k21);
	      for (int i = (ncmt == 1 ? (oral0 ? 5 : 2) : (oral0 ? 15 : 8)); i--;){
		aSave[i] = A[i];
	      }
	    }
	    // Reset all the rates
	    if (op->nlinR == 2){
	      rate[0]=0.0;
	      rate[1]=0.0;
	      r1=0; r2=0;
	    } else {
	      rate[0] = 0.0;
	      r1=0; r2=0;
	    }
	    Alast = Alast0;
	    tlast = 0;
	    curTime = tau;
	    double tinf, r0;
	    int doInf=0;
	    switch (whI){
	    case 0: {
	      // Get bolus dose
	      if (cmtOff == 0){
		b1 = amt*d_F;
	      } else {
		b2 = amt*d_F2;
	      }
	      ssTauD(A, ncmt, oral0, &tau, &b1, &b2, &d_ka,
		    &rx_k, &rx_k12, &rx_k21);
	    } break;
	    case 8: // Duration is modeled
	    case 9: { // Rate is modeled
	      if (whI == 9) {
		if (cmtOff == 0)  {
		  // Infusion to central compartment with oral dosing
		  r0 = d_rate1;
		  tinf = amt/r0*d_F;
		} else {
		  // Infusion to central compartment or depot
		  r0 = d_rate2;
		  tinf = amt/r0*d_F2;
		}
	      } else {
		// duration is modeled
		if (cmtOff == 0) {
		  // With oral dosing infusion to central compartment
		  tinf = d_dur1;
		  r0 = amt/tinf*d_F;
		} else {
		  // Infusion to compartment #1 or depot
		  tinf = d_dur2;
		  r0 = amt/tinf*d_F2;
		}
	      }
	      doInf=1;
	    } break;
	    case 1:
	    case 2: {
	      if (ISNA(amt)){
	      } else if (amt > 0) {
		doInf=1;
		unsigned int p;
		r0 = amt;
		tinf = _getDur(ind->ixds, ind, 1, &p);
		if (whI == 1){
		  // Duration changes
		  if (cmtOff == 0){
		    tinf *= d_F;
		  } else {
		    tinf *= d_F2;
		  }
		} else {
		  // Rate changes
		  if (cmtOff == 0){
		    r0 *= d_F;
		  } else {
		    r0 *= d_F2;
		  }
		}
	      }
	    }
	    }
	    if (doInf){
	      // Infusion steady state
	      if (tinf >= tau){
		ind->wrongSSDur=1;
		for (int i = ncmt + oral0; i--;){
		  A[i] += R_NaN;
		}
		extraAdvan=0;
	      } else {
		if (cmtOff == 0){
		  r1 = r0; r2 = 0;
		} else {
		  r1 = 0; r2 = r0;
		}
		ssRateTauD(A, ncmt, oral0, &tinf, &tau,
			   &r1, &r2, &d_ka,
			   &rx_k, &rx_k12, &rx_k21);
	      }
	    }
	    // Now calculate steady state
	    if (wh0 == 20) {
	      for (int i = (ncmt == 1 ? (oral0 ? 5 : 2) : (oral0 ? 15 : 8)); i--;){
		A[i] += aSave[i];
	      }
	    }
	    extraAdvan=0;
	  } else if (wh0 == 30) {
	    // Turning off a compartment; Not supported put everything to NaN
	    for (int i = (ncmt == 1 ? (oral0 ? 5 : 2) : (oral0 ? 15 : 8)); i--;){
	      A[i] = R_NaN;
	    }
	    extraAdvan=0;
	  }
	  // dosing to cmt
	  amt = ind->dose[ind->ixds];
	  switch (whI){
	  case 0: { // Bolus dose
	    // base dose
	    if (cmtOff == 0){
	      b1 = amt*d_F;
	    } else {
	      b2 = amt*d_F2;
	    }
	  } break;
	  case 4: {
	    doReplace = cmtOff+1;
	  } break;
	  case 5: {
	    doMultiply= cmtOff+1;
	  } break;
	  case 9: { // modeled rate.
	    // These are specified in the linCmt
	    if (cmtOff == 0)  {
	      // Infusion to central compartment with oral dosing
	      rateAdjust = d_rate1;
	    } else {
	      // Infusion to central compartment or depot
	      rateAdjust = d_rate2;
	    }
	    // Save rate turn off in next dose
	    doRate = cmtOff+1;
	  } break;
	  case 8: { // modeled duration. 
	    //InfusionRate[cmt] -= dose[ind->ixds+1];
	    if (cmtOff == 0) {
	      // With oral dosing infusion to central compartment
	      rateAdjust = amt/d_dur1*d_F;
	    } else {
	      // Infusion to compartment #1 or depot
	      rateAdjust = amt/d_dur2*d_F2;
	    }
	    doRate = cmtOff+1;
	  } break;
	  case 7:{ // End modeled rate
	    if (cmtOff == 0)  {
	      // Infusion to central compartment with oral dosing
	      rateAdjust = -d_rate1;
	    } else {
	      // Infusion to central compartment or depot
	      rateAdjust = -d_rate2;
	    }
	    doRate = cmtOff+1;
	  } break;
	  case 1: { // Begin infusion
	    rateAdjust = amt; // Amt is negative when turning off
	    doRate = cmtOff+1;
	  } break;
	  case 6: { // end modeled duration
	    if (cmtOff == 0) {
	      // With oral dosing infusion to central compartment
	      rateAdjust = -amt/d_dur1*d_F;
	    } else {
	      // Infusion to compartment #1 or depot
	      rateAdjust = -amt/d_dur2*d_F2;
	    }
	    doRate = cmtOff+1;
	  } break;
	  case 2: {
	    // In this case bio-availability changes the rate, but the duration remains constant.
	    // rate = amt/dur
	    if (cmtOff == 0){
	      rateAdjust = amt*d_F;
	    } else {
	      rateAdjust = amt*d_F2;
	    }
	    doRate = cmtOff+1;
	  } break;
	  }
	  if (wh0 == 40){
	    // Steady state infusion
	    // Now advance to steady state dosing
	    // These are easy to solve
	    if (op->nlinR == 2){
	      rate[0]=0.0;
	      rate[1]=0.0;
	    } else {
	      rate[0] = 0.0;
	    }
	    if (doRate == 1){
	      r1 = rateAdjust;
	      r2 = 0;
	    } else {
	      r1 = 0;
	      r2 = rateAdjust;
	    }
	    doRate=0;
	    ssRateD(A, ncmt, oral0, &r1, &r2,
		   &d_ka, &rx_k, &rx_k12, &rx_k21);
	    extraAdvan=0;
	  }    

	}
      }
      /* REprintf("evid: %d; wh: %d; cmt: %d; wh100: %d; whI: %d; wh0: %d; %f\n", */
      /* 	   evid, wh, cmt, wh100, whI, wh0, A[oral0]); */
      /* REprintf("curTime: t:%f, it: %f curTime:%f, tlast: %f, b1: %f ", t, it, curTime, tlast, b1); */
      if (extraAdvan){
	doAdvanD(A, Alast, tlast, // Time of last amounts
		 curTime, ncmt, oral0, &b1, &b2, &r1, &r2,
		 &d_ka, &rx_k, &rx_k12, &rx_k21);
      }
      if (doReplace){
	A[doReplace-1] = amt;
      } else if (doMultiply){
	A[doMultiply-1] *= amt;
      } else if (doRate){
	rate[doRate-1] += rateAdjust;
      } 
      ind->solved = idx;
    }
    if (!sameTime){
      // Compute the advan solution of a t outside of the mesh.
      Alast = A;
      A = Alast0;
      tlast = curTime;
      curTime = t;
      b1 = b2 = 0;
      doAdvanD(A, Alast, tlast, // Time of last amounts
	      curTime, ncmt, oral0, &b1, &b2, &r1, &r2,
	      &d_ka, &rx_k, &rx_k12, &rx_k21);
    }
    /* REprintf("t: %f %f %d %d\n", t, A[oral0], idx, ind->ix[idx]); */
    /* REprintf("%f,%f,%f\n", A[oral0], rx_v, A[oral0]/rx_v); */
    return derTrans(A, ncmt, trans, val, p1, v1, p2, p3,
		    p4, p5, d_tlag,  d_F,  d_rate1,  d_dur1,
		    d_ka, d_tlag2, d_F2, d_rate2, d_dur2);
  }
  return R_NaN;
}

double linCmtB(rx_solve *rx, unsigned int id,
	       double t, int linCmt,
	       int ncmt, int trans, int val,
	       double dd_p1, double dd_v1,
	       double dd_p2, double dd_p3,
	       double dd_p4, double dd_p5,
	       double dd_tlag, double dd_F,
	       double dd_rate, double dd_dur,
	       // oral extra parameters
	       double dd_ka, double dd_tlag2,
	       double dd_F2, double dd_rate2, double dd_dur2){
  switch (rx->sensType){
  case 1: // sensitivity
    return linCmtBB(rx, id, t, linCmt, ncmt, trans, val,
		    dd_p1, dd_v1, dd_p2, dd_p3,
		    dd_p4, dd_p5, dd_tlag, dd_F,
		    dd_rate, dd_dur, dd_ka, dd_tlag2, dd_F2,
		    dd_rate2, dd_dur2);
    break;
  case 2: // forward difference
    return linCmtD(rx, id, t, linCmt, ncmt, trans, val,
		    dd_p1, dd_v1, dd_p2, dd_p3,
		    dd_p4, dd_p5, dd_tlag, dd_F,
		    dd_rate, dd_dur, dd_ka, dd_tlag2, dd_F2,
		    dd_rate2, dd_dur2);
    break;
  case 3: //central difference
    return linCmtE(rx, id, t, linCmt, ncmt, trans, val,
		   dd_p1, dd_v1, dd_p2, dd_p3,
		   dd_p4, dd_p5, dd_tlag, dd_F,
		   dd_rate, dd_dur, dd_ka, dd_tlag2, dd_F2,
		   dd_rate2, dd_dur2);
  case 4: // symbolic advan
    return linCmtF(rx, id, t, linCmt, ncmt, trans, val,
		   dd_p1, dd_v1, dd_p2, dd_p3,
		   dd_p4, dd_p5, dd_tlag, dd_F,
		   dd_rate, dd_dur, dd_ka, dd_tlag2, dd_F2,
		   dd_rate2, dd_dur2);
  default:
    error("unsupported sensitivity");
  }
}
