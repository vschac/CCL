#include "ccl_utils.h"
#include "math.h"
#include "stdio.h"
#include "stdlib.h"
#include "string.h"
#include "gsl/gsl_errno.h"
#include "gsl/gsl_integration.h"
#include "gsl/gsl_sf_expint.h"
#include "gsl/gsl_roots.h"
#include "ccl_background.h"
#include "ccl_power.h"
#include "ccl_massfunc.h"
#include "ccl_error.h"
#include "ccl_halomod.h"

// Analytic FT of NFW profile, from Cooray & Sheth (2002; Section 3 of https://arxiv.org/abs/astro-ph/0206508)
// Normalised such that U(k=0)=1
static double u_nfw_c(ccl_cosmology *cosmo, double c, double halomass, double k, double a, int *status){
   
  double rv, rs, ks, Dv;
  double f1, f2, f3, fc;
  double Delta_v = Dv_BryanNorman(cosmo, a, status); // Virial density of haloes

  // Special case to prevent numerical problems if k=0,
  // the result should be unity here because of the normalisation
  if (k==0.) {    
    return 1.;    
  }

  // The general k case
  else{
    
    // Scale radius for NFW (rs=rv/c)
    rv = r_delta(cosmo, halomass, a, Delta_v, status);
    rs = rv/c;

    // Dimensionless wave-number variable
    ks = k*rs;

    // Various bits for summing together to get final result
    f1 = sin(ks)*(gsl_sf_Si(ks*(1.+c))-gsl_sf_Si(ks));
    f2 = cos(ks)*(gsl_sf_Ci(ks*(1.+c))-gsl_sf_Ci(ks));
    f3 = sin(c*ks)/(ks*(1.+c));
    fc = log(1.+c)-c/(1.+c);
  
    return (f1+f2-f3)/fc;
    
  }
}

/*----- ROUTINE: ccl_halo_concentration -----
INPUT: cosmology, a halo mass [Msun], scale factor, halo definition, concentration model label
TASK: Computes halo concentration; the ratio of virial raidus to scale radius for an NFW halo.
*/
double ccl_halo_concentration(ccl_cosmology *cosmo, double halomass, double a, double odelta, ccl_conc_label label, int *status){

  double gz, g0, nu, delta_c, a_form;
  double Mpiv, A, B, C;

  switch(label){

    // Bhattacharya et al. (2011; 1005.2239; Delta = 200rho_m; Table 2)
  case ccl_bhattacharya2011:

    if (odelta != 200.) {
      *status = CCL_ERROR_CONC_DV;
      strcpy(cosmo->status_message, "ccl_halomod.c: halo_concentration(): Bhattacharya (2011) concentration relation only valid for Delta_v = 200 \n");
      return NAN;
    }
    
    gz = ccl_growth_factor(cosmo,a,status);
    g0 = ccl_growth_factor(cosmo,1.0,status);
    delta_c = 1.686;
    nu = delta_c/ccl_sigmaM(cosmo, halomass, a, status);
    return 9.*pow(nu,-0.29)*pow(gz/g0,1.15);

    // Duffy et al. (2008; 0804.2486; Table 1, second section: Delta = Virial)
  case ccl_duffy2008_virial:

    if (odelta != Dv_BryanNorman(cosmo, a, status)) {
      *status = CCL_ERROR_CONC_DV;
      strcpy(cosmo->status_message, "ccl_halomod.c: halo_concentration(): Duffy (2008) virial concentration called with non-virial Delta_v\n");
      return NAN;
    }
    
    Mpiv = 2e12/cosmo->params.h; // Pivot mass in Msun (note in the paper units are Msun/h)
    A = 7.85;
    B = -0.081;
    C = -0.71;
    return A*pow(halomass/Mpiv,B)*pow(a,-C); 

    // Constant concentration (good for tests)
  case ccl_constant_concentration:
    
    return 4.;

    // Something went wrong
  default:
    
    *status = CCL_ERROR_HALOCONC;
    ccl_raise_exception(*status, "ccl_halomod.c: concentration-mass relation specified incorrectly");
    return NAN;
    	  
  }
}

// Fourier Transforms of halo profiles
static double window_function(ccl_cosmology *cosmo, double m, double k, double a, double odelta, ccl_win_label label, int *status){

  double rho_matter, c;
  
  switch(label){

  case ccl_nfw:

    // The mean background matter density in Msun/Mpc^3
    rho_matter = ccl_rho_x(cosmo, 1., ccl_species_m_label, 1, status);

    // The halo concentration for this mass and scale factor  
    c = ccl_halo_concentration(cosmo, m, a, odelta, ccl_duffy2008_virial, status);    

    // The function U is normalised so multiplying by M/rho turns units to overdensity
    return m*u_nfw_c(cosmo, c, m, k, a, status)/rho_matter;

    // Something went wrong
  default:
    
    *status = CCL_ERROR_HALOWIN;
    ccl_raise_exception(*status, "ccl_halomod.c: Window function specified incorrectly");
    return NAN;
    
  }
  
}

// Parameters structure for the one-halo integrand
typedef struct{  
  ccl_cosmology *cosmo;
  double k, a;
  int *status;
} Int_one_halo_Par;

// Integrand for the one-halo integral
static double one_halo_integrand(double log10mass, void *params){  
  
  Int_one_halo_Par *p = (Int_one_halo_Par *)params;;
  double halomass = pow(10,log10mass);
  double Delta_v = Dv_BryanNorman(p->cosmo, p->a, p->status); // Virial density of haloes

  // The squared normalised Fourier Transform of a halo profile (W(k->0 = 1)
  double wk = window_function(p->cosmo,halomass, p->k, p->a, Delta_v, ccl_nfw, p->status);

  // Fairly sure that there should be no ln(10) factor should be here since the integration is being specified in log10 range
  double dn_dlogM = ccl_massfunc(p->cosmo, halomass, p->a, Delta_v, p->status);
    
  return dn_dlogM*pow(wk,2);
}

// The one-halo term integral using gsl
static double one_halo_integral(ccl_cosmology *cosmo, double k, double a, int *status){

  int one_halo_integral_status = 0, qagstatus;
  double result = 0, eresult;
  double log10mmin = log10(HM_MMIN);
  double log10mmax = log10(HM_MMAX);
  Int_one_halo_Par ipar;
  gsl_function F;
  gsl_integration_workspace *w = gsl_integration_workspace_alloc(1000);

  // Structure required for the gsl integration
  ipar.cosmo = cosmo;
  ipar.k = k;
  ipar.a = a;
  ipar.status = &one_halo_integral_status;
  F.function = &one_halo_integrand;
  F.params = &ipar;

  // Actually does the integration
  qagstatus = gsl_integration_qag(&F, log10mmin, log10mmax, HM_EPSABS, HM_EPSREL, HM_LIMIT, HM_INT_METHOD, w, &result, &eresult);

  // Clean up
  gsl_integration_workspace_free(w);

  // Check for errors
  if (qagstatus != GSL_SUCCESS) {
    ccl_raise_gsl_warning(qagstatus, "ccl_halomod.c: one_halo_integral():");
    *status = CCL_ERROR_ONE_HALO_INT;
    sprintf(cosmo->status_message ,"ccl_halomod.c: one_halo_integral(): Integration failure\n");
    return NAN;      
  } else {
    return result;
  }
  
}

// Parameters structure for the two-halo integrand
typedef struct{  
  ccl_cosmology *cosmo;
  double k, a;
  int *status;
} Int_two_halo_Par;

// Integrand for the two-halo integral
static double two_halo_integrand(double log10mass, void *params){  
  
  Int_two_halo_Par *p=(Int_two_halo_Par *)params;
  double halomass = pow(10,log10mass);
  double Delta_v = Dv_BryanNorman(p->cosmo, p->a, p->status);

  // The window function appropriate for the matter power spectrum
  double wk = window_function(p->cosmo,halomass, p->k, p->a, Delta_v, ccl_nfw, p->status);

  // Fairly sure that there should be no ln(10) factor should be here since the integration is being specified in log10 range
  double dn_dlogM = ccl_massfunc(p->cosmo, halomass, p->a, Delta_v, p->status);

  // Halo bias
  double b = ccl_halo_bias(p->cosmo, halomass, p->a, Delta_v, p->status);
    
  return b*dn_dlogM*wk;
}

// The two-halo term integral using gsl
static double two_halo_integral(ccl_cosmology *cosmo, double k, double a, int *status){

  int two_halo_integral_status = 0, qagstatus;
  double result = 0, eresult;
  double log10mmin = log10(HM_MMIN);
  double log10mmax = log10(HM_MMAX);
  Int_two_halo_Par ipar;
  gsl_function F;
  gsl_integration_workspace *w = gsl_integration_workspace_alloc(1000);

  // Structure required for the gsl integration
  ipar.cosmo = cosmo;
  ipar.k = k;
  ipar.a = a;
  ipar.status = &two_halo_integral_status;
  F.function = &two_halo_integrand;
  F.params = &ipar;

  // Actually does the integration
  qagstatus = gsl_integration_qag(&F, log10mmin, log10mmax, HM_EPSABS, HM_EPSREL, HM_LIMIT, HM_INT_METHOD, w, &result, &eresult);

  // Clean up
  gsl_integration_workspace_free(w);

  // Check for errors
  if (qagstatus != GSL_SUCCESS) {
    ccl_raise_gsl_warning(qagstatus, "ccl_halomod.c: two_halo_integral():");
    *status = CCL_ERROR_TWO_HALO_INT;
    sprintf(cosmo->status_message ,"ccl_halomod.c: two_halo_integral(): Integration failure\n");
    return NAN;      
  } else {
    return result;
  }
  
}

/*----- ROUTINE: ccl_twohalo_matter_power -----
INPUT: cosmology, wavenumber [Mpc^-1], scale factor
TASK: Computes the two-halo power spectrum term in the halo model assuming NFW haloes
*/
double ccl_twohalo_matter_power(ccl_cosmology *cosmo, double k, double a, int *status){
    
  // Get the integral
  double I2h = two_halo_integral(cosmo, k, a, status);
    
  // The addative correction is the missing part of the integral below the lower-mass limit
  double A = 1.-two_halo_integral(cosmo, 0., a, status);

  // Virial overdensity
  double Delta_v = Dv_BryanNorman(cosmo, a, status);

  // ...multiplied by the ratio of window functions
  double W1 = window_function(cosmo, HM_MMIN, k,  a, Delta_v, ccl_nfw, status);
  double W2 = window_function(cosmo, HM_MMIN, 0., a, Delta_v, ccl_nfw, status);
  A = A*W1/W2;    

  // Add the additive correction to the calculated integral
  I2h = I2h+A;
      
  return ccl_linear_matter_power(cosmo, k, a, status)*I2h*I2h;
  
}

/*----- ROUTINE: ccl_onehalo_matter_power -----
INPUT: cosmology, wavenumber [Mpc^-1], scale factor
TASK: Computes the one-halo power spectrum term in the halo model assuming NFW haloes
*/
double ccl_onehalo_matter_power(ccl_cosmology *cosmo, double k, double a, int *status){  
  return one_halo_integral(cosmo, k, a, status);
}

/*----- ROUTINE: ccl_onehalo_matter_power -----
INPUT: cosmology, wavenumber [Mpc^-1], scale factor
TASK: Computes the halo model power spectrum by summing the two- and one-halo terms
*/
double ccl_halomodel_matter_power(ccl_cosmology *cosmo, double k, double a, int *status){  
  // Standard sum of two- and one-halo terms
  return ccl_twohalo_matter_power(cosmo, k, a, status)+ccl_onehalo_matter_power(cosmo, k, a, status);   
}
