
//
// $Id: Godunov.cpp,v 1.9 1998-05-13 23:17:48 almgren Exp $
//

//==========================================================
// Godunov is the object which calculates advective terms
// for iamr
//==========================================================

#include <Misc.H>
#include <LO_BCTYPES.H>
#include <CoordSys.H>
#include <Geometry.H>
#include <ParmParse.H>
#include <BoxDomain.H>
#include <FArrayBox.H>
#include <Godunov.H>
#include <RunStats.H>
#include <GODUNOV_F.H>

#define GEOM_GROW 1
#define HYP_GROW  3
#define XVEL 0
#define YVEL 1
#define ZVEL 2

#if (BL_SPACEDIM == 3 )
#define ADD_W
#else
#undef ADD_W
#endif

// why because, I like my macro the best.  DS
#if (BL_SPACEDIM == 2 )
#define GDIMS(l,h) &l[0], &l[1], &h[0], &h[1] 
#else
#define GDIMS(l,h) &l[0], &l[1], &l[2], &h[0], &h[1], &h[2]
#endif

int Godunov::verbose = 0;
int Godunov::slope_order = 4;
int Godunov::use_forces_in_trans = 0;

// =============================================================
// Setup functions follow
// =============================================================

// initialization of static members
#define bogus_value 1.e20

// construct the Godunov Object
Godunov::Godunov() :
        max_1d(0)
{
    read_params();
    // set to 512 initiallly
    ZeroScratch();
    SetScratch( 512 );
}

// size the 1D workspace explicitly
Godunov::Godunov( int max_size ) :
        max_1d(max_size)
{
    read_params();
    ZeroScratch();
    SetScratch( max_size );
}

// read parameters from input file and command line
void Godunov::read_params()
{
  // read parameters from input file and command line
  ParmParse pp("godunov");

  pp.query("v",verbose);

  pp.query("slope_order",slope_order);
#if (BL_SPACEDIM==2)
  assert(slope_order==1 || slope_order==2 || slope_order==4);
#else
  assert(slope_order==1 || slope_order==4);
#endif
  pp.query("use_forces_in_trans",use_forces_in_trans);

  FORT_SET_PARAMS(slope_order);
}


// set 1d scratch space as empty
void Godunov::ZeroScratch()
{
    stxlo  = NULL;
    stxhi  = NULL;
    slxscr = NULL;
                 
    stylo  = NULL;
    styhi  = NULL;
    slyscr = NULL;
#ifdef ADD_W     
    stzlo  = NULL;
    stzhi  = NULL;
    slzscr = NULL;
#endif
}


// initialize 1d scratch space to a bogus value
void Godunov::SetBogusScratch()
{
    for ( int i = 0 ; i < scr_size ; i++ ) {
        
        stxlo[i] = bogus_value;
        stxhi[i]  = bogus_value;
        slxscr[i] = bogus_value;
        
        stylo[i]  = bogus_value;
        styhi[i]  = bogus_value;
        slyscr[i] = bogus_value;
#ifdef ADD_W     
        stzlo[i]  = bogus_value;
        stzhi[i]  = bogus_value;
        slzscr[i] = bogus_value;
#endif
    }
}



// set 1d scratch space
void Godunov::SetScratch( int max_size )
{
    // set sizing parameters
    if ( max_size < max_1d )
        return;
    else
        max_1d = Max(max_1d,max_size);
    scr_size = ( max_size+2*HYP_GROW)*4;

    // get rid of the old scratch space
    RemScratch();
    
    // construct arrays
    stxlo  = new Real[scr_size];
    stxhi  = new Real[scr_size];
    slxscr = new Real[scr_size];
    
    stylo  = new Real[scr_size];
    styhi  = new Real[scr_size];
    slyscr = new Real[scr_size];
#ifdef ADD_W
    stzlo  = new Real[scr_size];
    stzhi  = new Real[scr_size];
    slzscr = new Real[scr_size];
#endif
}


// remove 1D scratch space
void Godunov::RemScratch()
{
    delete [] stxlo;
    delete [] stxhi;
    delete [] slxscr;
    
    delete [] stylo;
    delete [] styhi;
    delete [] slyscr;
#ifdef ADD_W    
    delete [] stzlo;
    delete [] stzhi;
    delete [] slzscr; 
#endif
}

// destructor destroys work arrays
Godunov::~Godunov()
{
    RemScratch();
}



// set up the farrayboxes for computing edge states, This function
// returns, the Farrayboxes where the fluxes are stored as these
// are used in NavierStokes
//
// It also computes the transverse advective velocities
//
// The amount of workspace needed in FArrayBox work is currently
// 2*SDIM+1
//
void Godunov::Setup( const Box &grd, const Real *dx, Real dt, int velpred,
                     FArrayBox &xflux, const int *ubc,
                     FArrayBox &yflux, const int *vbc,
#ifdef ADD_W
                     FArrayBox &zflux, const int *wbc,
#endif
                     FArrayBox &U, FArrayBox &rho, 
                     const FArrayBox& tforces )
{
    assert( rho.nComp()  == 1           );
    assert( U.nComp()    >= BL_SPACEDIM );

    // compute the edge boxes
    xflux_bx = Box(grd);
    xflux_bx.surroundingNodes(0);
    yflux_bx = Box(grd);
    yflux_bx.surroundingNodes(1);
#ifdef ADD_W
    zflux_bx = Box(grd);
    zflux_bx.surroundingNodes(2);
#endif
    
    // create storage for fluxes
    if ( !velpred ) {
        xflux.resize(xflux_bx,1);
        yflux.resize(yflux_bx,1);
#ifdef ADD_W
        zflux.resize(zflux_bx,1);
#endif
    }

    // ensure 1D scratch space is large enough
    Box test_bx(grow(grd,HYP_GROW));
    SetScratch( test_bx.longside() );

    // create the private advective velocities and the
    // FAB workspace for the GODUNOV Box
    work_bx = Box(grow(grd,1));
    work.resize(work_bx,2*BL_SPACEDIM+1);
    uad.resize(work_bx,1);
    vad.resize(work_bx,1);
#ifdef ADD_W 
    wad.resize(work_bx,1);
#endif

    // set work variables to bogus values
    SetBogusScratch();
    xflux.setVal( bogus_value );
    yflux.setVal( bogus_value );
    uad.setVal(   bogus_value );
    vad.setVal(   bogus_value );
#ifdef ADD_W
    zflux.setVal( bogus_value );
    wad.setVal(   bogus_value );
#endif
    work.setVal(  bogus_value );
    
    // ---------------------------- test the cell-centered velocities
    // Real u_max[3];
    // test_u_rho( S, rho, grd, dx, dt, u_max );
    
    // ---------------------------- create the bounds and pointers
    const int *lo    = grd.loVect();
    const int *hi    = grd.hiVect();
    const int *u_lo  = U.loVect();
    const int *u_hi  = U.hiVect();
    const int *w_lo = work.loVect();
    const int *w_hi = work.hiVect();

    const Real *u_dat   = U.dataPtr(XVEL);
    const Real *uad_dat = uad.dataPtr();
    const Real *v_dat   = U.dataPtr(YVEL);
    const Real *vad_dat = vad.dataPtr();
#ifdef ADD_W
    const Real *w_dat   = U.dataPtr(ZVEL);
    const Real *wad_dat = wad.dataPtr();
    const Real *xhi_dat = work.dataPtr(1);
    const Real *yhi_dat = work.dataPtr(2);
    const Real *zhi_dat = work.dataPtr(3);
    const Real *slx_dat = work.dataPtr(4);
    const Real *sly_dat = work.dataPtr(5);
    const Real *slz_dat = work.dataPtr(6);
#else
    const Real *xhi_dat = work.dataPtr(1);
    const Real *yhi_dat = work.dataPtr(2);
    const Real *slx_dat = work.dataPtr(3);
    const Real *sly_dat = work.dataPtr(4);
#endif 

    const Real* tforcedat = NULL;
    if(use_forces_in_trans) {
      tforcedat = tforces.dataPtr();
    }
    
    // ---------------------------- compute the transverse velocities
    FORT_TRANSVEL( u_dat, uad_dat, xhi_dat, slx_dat, ubc, slxscr, 
                   v_dat, vad_dat, yhi_dat, sly_dat, vbc, slyscr, 
#ifdef ADD_W
                   w_dat, wad_dat, zhi_dat, slz_dat, wbc, slzscr,
#endif    
                   GDIMS(u_lo,u_hi),
                   GDIMS(w_lo,w_hi), 
                   lo, hi, &dt, dx, &use_forces_in_trans, tforcedat);
}

// =============================================================
// Advection functions follow
// =============================================================


// compute the edge states using the advective transverse velocities
// The amount of workspace needed in FArrayBox work is currently
// 2*SDIM+1
void Godunov::edge_states( const Box &grd, const Real *dx, Real dt, int velpred,
                           FArrayBox &uedge, FArrayBox &stx,
                           FArrayBox &vedge, FArrayBox &sty,
#ifdef ADD_W               
                           FArrayBox &wedge, FArrayBox &stz,
#endif
                           FArrayBox &U, FArrayBox &S, FArrayBox &tforces,
                           int fab_ind, int state_ind, const int *bc )
{
    // ---------------------------- error block
    Box tst = S.box();
    assert( tst.contains( work_bx ) );

    assert( U.nComp()       >= BL_SPACEDIM );
    assert( S.nComp()       >= fab_ind     );
    assert( tforces.nComp() >= fab_ind     );

    assert( uedge.box()     == xflux_bx    );
    assert( uedge.nComp()   >= 1           );
    assert( stx.nComp()     >= 1           );

    assert( vedge.box()     == yflux_bx    );
    assert( vedge.nComp()   >= 1           );
    assert( sty.nComp()     >= 1           );
#ifdef ADD_W
    assert( wedge.box()     == zflux_bx    );
    assert( wedge.nComp()   >= 1           );
    assert( stz.nComp()     >= 1           );
#endif    

    
    // ---------------------------- create the bounds and pointers
    const int *lo    = grd.loVect();
    const int *hi    = grd.hiVect();
    const int *s_lo  = S.loVect();
    const int *s_hi  = S.hiVect();
    const int *u_lo  = uedge.loVect();
    const int *u_hi  = uedge.hiVect();
    const int *v_lo  = vedge.loVect();
    const int *v_hi  = vedge.hiVect();
    const int *ww_lo = work.loVect();
    const int *ww_hi = work.hiVect();

    const Real *s_dat   = S.dataPtr(fab_ind);
    const Real *u_dat   = U.dataPtr(XVEL);
    const Real *v_dat   = U.dataPtr(YVEL);
    
    const Real *tfr_dat = tforces.dataPtr(fab_ind);
    
    const Real *uad_dat   = uad.dataPtr();
    const Real *vad_dat   = vad.dataPtr();
    const Real *uedge_dat = uedge.dataPtr();
    const Real *vedge_dat = vedge.dataPtr();
    const Real *stx_dat   = stx.dataPtr();
    const Real *sty_dat   = sty.dataPtr();

    // set work space to bogus values
    work.setVal(  bogus_value );
    SetBogusScratch();

#ifdef ADD_W
    const int *w_lo = wedge.loVect();
    const int *w_hi = wedge.hiVect();
    const Real *w_dat     = U.dataPtr(ZVEL);
    const Real *wad_dat   = wad.dataPtr();
    const Real *wedge_dat = wedge.dataPtr();
    const Real *stz_dat   = stz.dataPtr();

    const Real *xhi_dat = work.dataPtr(0);
    const Real *yhi_dat = work.dataPtr(0);
    const Real *zhi_dat = work.dataPtr(0);
    const Real *xlo_dat = work.dataPtr(1);
    const Real *ylo_dat = work.dataPtr(2);
    const Real *zlo_dat = work.dataPtr(3);
    const Real *slx_dat = work.dataPtr(4);
    const Real *sly_dat = work.dataPtr(5);
    const Real *slz_dat = work.dataPtr(6);
#else
    const Real *xhi_dat = work.dataPtr(0);
    const Real *yhi_dat = work.dataPtr(0);
    const Real *xlo_dat = work.dataPtr(1);
    const Real *ylo_dat = work.dataPtr(2);
    const Real *slx_dat = work.dataPtr(3);
    const Real *sly_dat = work.dataPtr(4);
#endif
    // C component indices starts from 0, Fortran from 1
    int fort_ind = state_ind+1;  

    FORT_ESTATE(s_dat, tfr_dat, GDIMS(s_lo,s_hi),

                u_dat, xlo_dat, xhi_dat, slx_dat, uad_dat,
                slxscr, stxlo, stxhi,
                uedge_dat, stx_dat, GDIMS(u_lo,u_hi), 

                v_dat, ylo_dat, yhi_dat, sly_dat, vad_dat,
                slyscr, stylo, styhi,
                vedge_dat, sty_dat, GDIMS(v_lo,v_hi), 
#ifdef ADD_W
                w_dat, zlo_dat, zhi_dat, slz_dat, wad_dat,
                slzscr, stzlo, stzhi,
                wedge_dat, stz_dat, GDIMS(w_lo,w_hi), 
#endif
                GDIMS(ww_lo,ww_hi),
                bc, lo, hi, &dt, dx, &fort_ind, &velpred, 
                &use_forces_in_trans);

}


// compute the edge states for The Mac projection
// FArrayBox work sized as in edge_states
void Godunov::ComputeUmac( const Box &grd, const Real *dx, Real dt, 
                           FArrayBox &umac, const int *ubc, 
                           FArrayBox &vmac, const int *vbc, 
#ifdef ADD_W
                           FArrayBox &wmac, const int *wbc, 
#endif
                           FArrayBox &U, FArrayBox &tforces )
{
    int velpred = 1;

    //--------------------------- 2D calls
#ifndef ADD_W                  
    edge_states( grd, dx, dt, velpred,
                 umac, umac,
                 vmac, vmac,
                 U, U, tforces, XVEL, XVEL, ubc );
    edge_states( grd, dx, dt, velpred,
                 umac, umac,
                 vmac, vmac,
                 U, U, tforces, YVEL, YVEL, vbc );
#endif

    //--------------------------- 3D calls
#ifdef ADD_W                  
    edge_states( grd, dx, dt, velpred,
                 umac, umac,
                 vmac, vmac,
                 wmac, wmac,
                 U, U, tforces, XVEL, XVEL, ubc );
    edge_states( grd, dx, dt, velpred,
                 umac, umac,
                 vmac, vmac,
                 wmac, wmac,
                 U, U, tforces, YVEL, YVEL, vbc );
    edge_states( grd, dx, dt, velpred,
                 umac, umac,
                 vmac, vmac,
                 wmac, wmac,
                 U, U, tforces, ZVEL, ZVEL, wbc );
#endif
}




// advect a state component.
// This routine assumes uad,vad,wad have been precomputed
// FArrayBox work sized as in edge_states
void Godunov::AdvectState( const Box &grd, const Real *dx, Real dt, 
                           FArrayBox &areax, FArrayBox &uedge, FArrayBox &xflux,  
                           FArrayBox &areay, FArrayBox &vedge, FArrayBox &yflux,  
#ifdef ADD_W                               
                           FArrayBox &areaz, FArrayBox &wedge, FArrayBox &zflux,
#endif
                           FArrayBox &U,
                           FArrayBox &S, FArrayBox &tforces, int fab_ind,
                           FArrayBox &aofs,                  int aofs_ind,
                           int iconserv, int state_ind, const int *bc,
                           FArrayBox &vol )
{
    int velpred = 0;

    // compute edge states for an advected quantity
    edge_states( grd, dx, dt, velpred,
                 uedge, xflux,
                 vedge, yflux,
#ifdef ADD_W             
                 wedge, zflux,
#endif
                 U, S, tforces, fab_ind, state_ind, bc );

    // compute the advective tendancy
    ComputeAofs( grd,
                 areax, uedge, xflux,  
                 areay, vedge, yflux,  
#ifdef ADD_W                             
                 areaz, wedge, zflux,
#endif                     
                 vol, aofs, aofs_ind, iconserv );
                 
}




// compute the advective derivative from fluxes
void Godunov::ComputeAofs( const Box &grd, 
                           FArrayBox &areax, FArrayBox &uedge, FArrayBox &xflux,  
                           FArrayBox &areay, FArrayBox &vedge, FArrayBox &yflux,  
#ifdef ADD_W                               
                           FArrayBox &areaz, FArrayBox &wedge, FArrayBox &zflux,
#endif
                           FArrayBox &vol,
                           FArrayBox &aofs,  int aofs_ind, int iconserv )
{
    // ---------------------------- create the bounds and pointers
    const int *lo    = grd.loVect();
    const int *hi    = grd.hiVect();

    aofs.setVal(bogus_value,aofs_ind);
    const int *a_lo  = aofs.loVect();
    const int *a_hi  = aofs.hiVect();
    const Real *aofs_dat = aofs.dataPtr(aofs_ind);

    const int *ax_lo  = areax.loVect();
    const int *ax_hi  = areax.hiVect();
    const Real *areax_dat = areax.dataPtr();

    const int *ay_lo  = areay.loVect();
    const int *ay_hi  = areay.hiVect();
    const Real *areay_dat = areay.dataPtr();

    const int *v_lo  = vol.loVect();
    const int *v_hi  = vol.hiVect();
    const Real *vol_dat  = vol.dataPtr();
    
    const int *xflux_lo = xflux.loVect();
    const int *xflux_hi = xflux.hiVect();
    const Real *xflux_dat = xflux.dataPtr();
    const Real *uedge_dat = uedge.dataPtr();
    
    const int *yflux_lo = yflux.loVect();
    const int *yflux_hi = yflux.hiVect();
    const Real *yflux_dat = yflux.dataPtr();
    const Real *vedge_dat = vedge.dataPtr();
    
#ifdef ADD_W
    const int *az_lo  = areaz.loVect();
    const int *az_hi  = areaz.hiVect();
    const Real *areaz_dat = areaz.dataPtr();

    const int *zflux_lo = zflux.loVect();
    const int *zflux_hi = zflux.hiVect();
    const Real *zflux_dat = zflux.dataPtr();
    const Real *wedge_dat = wedge.dataPtr();
#endif
    
    // ---------------------------- compute the advective tendency
    FORT_ADV_FORCING( aofs_dat, GDIMS(a_lo,a_hi),
                      xflux_dat, uedge_dat, GDIMS(xflux_lo,xflux_hi),
                      areax_dat, GDIMS(ax_lo,ax_hi),
                      yflux_dat, vedge_dat, GDIMS(yflux_lo,yflux_hi),
                      areay_dat, GDIMS(ay_lo,ay_hi),
#ifdef ADD_W                                                    
                      zflux_dat, wedge_dat, GDIMS(zflux_lo,zflux_hi),
                      areaz_dat, GDIMS(az_lo,az_hi),
#endif
                      vol_dat, GDIMS(v_lo,v_hi),
                      lo, hi, &iconserv );
}



// sync advect a state component
// This routine assumes uad,vad,wad have been precomputed
void Godunov::SyncAdvect( const Box &grd, const Real *dx, Real dt, int level,
                          FArrayBox &areax, FArrayBox &uedge,
                          FArrayBox &ucorr, FArrayBox &xflux,
                          FArrayBox &areay, FArrayBox &vedge,
                          FArrayBox &vcorr, FArrayBox &yflux,
#ifdef ADD_W
                          FArrayBox &areaz, FArrayBox &wedge,
                          FArrayBox &wcorr, FArrayBox &zflux,
#endif
                          FArrayBox &S, FArrayBox &tforces, int fab_ind,
                          FArrayBox &sync,                  int sync_ind,
                          int iconserv, int state_ind, const int *bc,
                          FArrayBox &vol )
{
    int velpred = 0;

    // ---------------------------- error block
    Box tst = S.box();
    assert( tst.contains( work_bx ) );

    assert( S.nComp()       >= BL_SPACEDIM );
    assert( S.nComp()       >= fab_ind     );
    assert( tforces.nComp() >= fab_ind     );
    assert( sync.nComp()    >= sync_ind    );

    assert( ucorr.box()     == xflux_bx    );
    assert( ucorr.nComp()   >= 1           );

    assert( vcorr.box()     == yflux_bx    );
    assert( vcorr.nComp()   >= 1           );
#ifdef ADD_W
    assert( wcorr.box()     == zflux_bx    );
    assert( wcorr.nComp()   >= 1           );
#endif    

    // ---------------------------- compute the edge states
    edge_states( grd, dx, dt, velpred,
                 uedge, xflux,
                 vedge, yflux,
#ifdef ADD_W     
                 wedge, zflux,
#endif
                 S, S, tforces, fab_ind, state_ind, bc );

    // compute the advective tendancy for the mac sync
    ComputeSyncAofs( grd,
                     areax, ucorr, xflux,  
                     areay, vcorr, yflux,  
#ifdef ADD_W                             
                     areaz, wcorr, zflux,
#endif                     
                     vol, sync, sync_ind, iconserv );
}



// compute the advective derivative of corrective fluxes for the mac sync
void Godunov::ComputeSyncAofs( const Box &grd,
                               FArrayBox &areax, FArrayBox &ucorr, FArrayBox &xflux,  
                               FArrayBox &areay, FArrayBox &vcorr, FArrayBox &yflux,  
#ifdef ADD_W                             
                               FArrayBox &areaz, FArrayBox &wcorr, FArrayBox &zflux,
#endif                     
                               FArrayBox &vol,
                               FArrayBox &sync,
                               int sync_ind, int iconserv )
{
    // ---------------------------- create the bounds and pointers
    const int *lo    = grd.loVect();
    const int *hi    = grd.hiVect();
    
    const int *s_lo  = sync.loVect();
    const int *s_hi  = sync.hiVect();
    const Real *sync_dat = sync.dataPtr(sync_ind);
    
    const int *ax_lo  = areax.loVect();
    const int *ax_hi  = areax.hiVect();
    const Real *areax_dat = areax.dataPtr();

    const int *ay_lo  = areay.loVect();
    const int *ay_hi  = areay.hiVect();
    const Real *areay_dat = areay.dataPtr();

    const int *v_lo  = vol.loVect();
    const int *v_hi  = vol.hiVect();
    const Real *vol_dat  = vol.dataPtr();
    
    const int *xflux_lo = xflux.loVect();
    const int *xflux_hi = xflux.hiVect();
    const Real *xflux_dat = xflux.dataPtr();
    const Real *ucorr_dat = ucorr.dataPtr();
    
    const int *yflux_lo = yflux.loVect();
    const int *yflux_hi = yflux.hiVect();
    const Real *yflux_dat = yflux.dataPtr();
    const Real *vcorr_dat = vcorr.dataPtr();

#ifdef ADD_W
    const int *az_lo  = areaz.loVect();
    const int *az_hi  = areaz.hiVect();
    const Real *areaz_dat = areaz.dataPtr();

    const int *zflux_lo = zflux.loVect();
    const int *zflux_hi = zflux.hiVect();
    const Real *zflux_dat = zflux.dataPtr();
    const Real *wcorr_dat = wcorr.dataPtr();
#endif
    
    // ---------------------------- compute the corrective tendency
    FORT_SYNC_ADV_FORCING( sync_dat, GDIMS(s_lo,s_hi),
                           
                           xflux_dat, ucorr_dat, GDIMS(xflux_lo,xflux_hi),
                           areax_dat, GDIMS(ax_lo,ax_hi),
                           
                           yflux_dat, vcorr_dat, GDIMS(yflux_lo,yflux_hi),
                           areay_dat, GDIMS(ay_lo,ay_hi),
#ifdef ADD_W                                             
                           zflux_dat, wcorr_dat, GDIMS(zflux_lo,zflux_hi),
                           areaz_dat, GDIMS(az_lo,az_hi),
#endif
                           vol_dat, GDIMS(v_lo,v_hi),
                           lo, hi, &iconserv );
}    




// correct a scalar for under-over shoots
void Godunov::ScalMinMax( FArrayBox &Sold, FArrayBox &Snew, int ind, 
                          const int *bc, const Box &grd )
{
    const int *slo = Sold.loVect();
    const int *shi = Sold.hiVect();
    const int *lo  = grd.loVect();
    const int *hi  = grd.hiVect();
    const Real *Sold_dat = Sold.dataPtr(ind);
    const Real *Snew_dat = Snew.dataPtr(ind);

#ifdef ADD_W
    Box flatbox(grd);
    int zlen = flatbox.length()[BL_SPACEDIM-1];
    flatbox.growHi(BL_SPACEDIM-1,3-zlen);
    FArrayBox smin(flatbox,1);
    FArrayBox smax(flatbox,1);
    const Real *smin_dat = smin.dataPtr();
    const Real *smax_dat = smax.dataPtr(); 
#endif

    FORT_SCALMINMAX (Sold_dat, GDIMS(slo,shi),
                     Snew_dat, GDIMS(slo,shi),
#ifdef ADD_W
                     smin_dat, smax_dat,
                     GDIMS(lo,hi),
#endif
                     lo, hi, bc);
}



// =============================================================
// Diagnostic functions follow
// =============================================================

//
// estimate the maximum allowable timestep at a cell center
//
Real Godunov::estdt( FArrayBox &U, FArrayBox &tforces, FArrayBox &rho,
                     const Box &grd, const Real *dx, Real cfl, Real *u_max )
{
    assert( U.nComp()       >= BL_SPACEDIM );
    assert( tforces.nComp() >= BL_SPACEDIM );
    assert( rho.nComp()     == 1           );

    const int *lo  = grd.loVect();
    const int *hi  = grd.hiVect();
    const int *vlo = U.loVect();
    const int *vhi = U.hiVect();
    const int *tlo = tforces.loVect();
    const int *thi = tforces.hiVect();
    const int *rlo = rho.loVect();
    const int *rhi = rho.hiVect();
    
    const Real *Udat  = U.dataPtr();
    const Real *tfdat = tforces.dataPtr();
    const Real *rdat  = rho.dataPtr();

    Real dt;
    FORT_ESTDT( Udat,  GDIMS(vlo,vhi),
                tfdat, GDIMS(tlo,thi),
                rdat,  GDIMS(rlo,rhi),
                lo, hi, &dt, dx, &cfl, u_max );
    return dt;
}



//
// estimate the extrema of cell-centered us and rho
//
Real Godunov::test_u_rho( FArrayBox &U, FArrayBox &rho, 
                          const Box &grd, const Real *dx, const Real dt,
                          const Real *u_max )
{
    assert( U.nComp()   >= BL_SPACEDIM );
    assert( rho.nComp() == 1           );
    
    const int *lo  = grd.loVect();
    const int *hi  = grd.hiVect();
    const int *vlo = U.loVect();
    const int *vhi = U.hiVect();
    const int *rlo = rho.loVect();
    const int *rhi = rho.hiVect();

    const Real *rh = rho.dataPtr();
    const Real *u  = U.dataPtr(XVEL);
    const Real *v  = U.dataPtr(YVEL);
#ifdef ADD_W
    const Real *w  = U.dataPtr(ZVEL);
#endif

    Real cflmax = 0;
    FORT_TEST_U_RHO( u,  GDIMS(vlo,vhi),
                     v,  GDIMS(vlo,vhi),
#ifdef ADD_W                          
                     w,  GDIMS(vlo,vhi),
#endif
                     rh, GDIMS(rlo,rhi),
                     lo, hi, &dt, dx, &cflmax, u_max, &verbose);
    return cflmax;
}



//
// estimate the extrema of umac edge velocities and rho
//
Real Godunov::test_umac_rho( FArrayBox &umac,
                             FArrayBox &vmac,
#ifdef ADD_W
                             FArrayBox &wmac,
#endif
                             FArrayBox &rho,
                             const Box &grd, const Real *dx, const Real dt,
                             const Real *u_max )
{
    // test block
    assert( umac.nComp() == 1 );
    assert( vmac.nComp() == 1 );
#ifdef ADD_W
    assert( wmac.nComp() == 1 );
#endif
    assert( rho.nComp()  == 1 );
    
    const int *lo  = grd.loVect();
    const int *hi  = grd.hiVect();
    const int *ulo = umac.loVect();
    const int *uhi = umac.hiVect();
    const int *vlo = vmac.loVect();
    const int *vhi = vmac.hiVect();
    const int *rlo = rho.loVect();
    const int *rhi = rho.hiVect();
    const Real *um = umac.dataPtr();
    const Real *vm = vmac.dataPtr();
    const Real *rh = rho.dataPtr();
    
#ifdef ADD_W
    const int *wlo = wmac.loVect();
    const int *whi = wmac.hiVect();
    const Real *wm = wmac.dataPtr();
#endif

    Real cfl;
    FORT_TEST_UMAC_RHO( um, GDIMS(ulo,uhi),
                        vm, GDIMS(vlo,vhi),
#ifdef ADD_W                            
                        wm, GDIMS(wlo,whi),
#endif                                              
                        rh, GDIMS(rlo,rhi),
                        lo, hi, &dt, dx, &cfl, u_max );
    return cfl;
}




// =============================================================
// Source term functions follow
// =============================================================


// compute the update rule, this is useful for 1st order RK
//
// psi^n+1 = psi^n + dt*tf^n
//
void Godunov::Add_tf( FArrayBox &Sold,
                      FArrayBox &Snew,    int start_ind, int num_comp, 
                      FArrayBox &tforces, int tf_ind,
                      const Box &grd,     Real dt )
{
    assert( Snew.nComp()    >= start_ind + num_comp );
    assert( Sold.nComp()    >= start_ind + num_comp );
    assert( tforces.nComp() >= tf_ind    + num_comp );

    const int *slo = Sold.loVect();
    const int *shi = Sold.hiVect();
    const int *tlo = tforces.loVect();
    const int *thi = tforces.hiVect();
    const int *lo  = grd.loVect();
    const int *hi  = grd.hiVect();
    const Real *SOdat = Sold.dataPtr(start_ind);
    const Real *SNdat = Snew.dataPtr(start_ind);
    const Real *TFdat = tforces.dataPtr(tf_ind);
    
    FORT_UPDATE_TF( SOdat, GDIMS(slo,shi), 
                    SNdat, GDIMS(slo,shi),
                    TFdat, GDIMS(tlo,thi),
                    lo, hi, &dt, &num_comp );
}


// correct the 1st order RK to 2nd order via
//
// psi^n+1 = psi^* + (dt/2)*(tf^* - tf^n)
//
void Godunov::Correct_tf( FArrayBox &Sstar, FArrayBox &Snp1,
                          int start_ind, int num_comp, 
                          FArrayBox &tfstar, FArrayBox &tfn,
                          int tf_ind,
                          const Box &grd,     Real dt )
{
    assert( Snp1.nComp()   >= start_ind + num_comp );
    assert( Sstar.nComp()  >= start_ind + num_comp );
    assert( tfstar.nComp() >= tf_ind    + num_comp );
    assert( tfn.nComp()    >= tf_ind    + num_comp );

    const int *slo = Sstar.loVect();
    const int *shi = Sstar.hiVect();
    const int *tlo = tfstar.loVect();
    const int *thi = tfstar.hiVect();
    const int *lo  = grd.loVect();
    const int *hi  = grd.hiVect();
    const Real *SSdat = Sstar.dataPtr(start_ind);
    const Real *SPdat = Snp1.dataPtr(start_ind);
    const Real *TSdat = tfstar.dataPtr(tf_ind);
    const Real *TNdat = tfn.dataPtr(tf_ind);
    
    FORT_CORRECT_TF( SSdat, SPdat, GDIMS(slo,shi),
                     TSdat, TNdat, GDIMS(tlo,thi),
                     lo, hi, &dt, &num_comp );
}



// compute the update rule
//
// psi^n+1 = psi^n - dt*aofs + dt*tforces
//
void Godunov::Add_aofs_tf( FArrayBox &Sold,
                           FArrayBox &Snew,    int start_ind, int num_comp,
                           FArrayBox &Aofs,    int aofs_ind,
                           FArrayBox &tforces, int tf_ind,
                           const Box &grd,     Real dt )
{
    assert( Snew.nComp()    >= start_ind + num_comp );
    assert( Sold.nComp()    >= start_ind + num_comp );
    assert( Aofs.nComp()    >= aofs_ind  + num_comp );
    assert( tforces.nComp() >= tf_ind    + num_comp );

    const int *slo = Sold.loVect();
    const int *shi = Sold.hiVect();
    const int *alo = Aofs.loVect();
    const int *ahi = Aofs.hiVect();
    const int *tlo = tforces.loVect();
    const int *thi = tforces.hiVect();
    const int *lo  = grd.loVect();
    const int *hi  = grd.hiVect();
    const Real *SOdat = Sold.dataPtr(start_ind);
    const Real *SNdat = Snew.dataPtr(start_ind);
    const Real *AOdat = Aofs.dataPtr(aofs_ind);
    const Real *TFdat = tforces.dataPtr(tf_ind);
    
    FORT_UPDATE_AOFS_TF( SOdat, GDIMS(slo,shi), 
                         SNdat, GDIMS(slo,shi),
                         AOdat, GDIMS(alo,ahi),
                         TFdat, GDIMS(tlo,thi),
                         lo, hi, &dt, &num_comp );
}




// compute the update rule for velocities
//
// psi^n+1 = psi^n - dt*aofs - dt*gp/rho + dt*tforces
//
void Godunov::Add_aofs_tf_gp( FArrayBox &Uold, FArrayBox &Unew,
                              FArrayBox &Aofs, FArrayBox &tforces,
                              FArrayBox &gp,   FArrayBox &rho, 
                              const Box &grd,  Real dt )
{
    assert( Unew.nComp()    >= BL_SPACEDIM );
    assert( Uold.nComp()    >= BL_SPACEDIM );
    assert( Aofs.nComp()    >= BL_SPACEDIM );
    assert( tforces.nComp() >= BL_SPACEDIM );
    assert( gp.nComp()      == BL_SPACEDIM );
    assert( rho.nComp()     == 1           );
    
    const int *lo  = grd.loVect();
    const int *hi  = grd.hiVect();
    const int *ulo = Uold.loVect();
    const int *uhi = Uold.hiVect();
    const int *alo = Aofs.loVect();
    const int *ahi = Aofs.hiVect();
    const int *tlo = tforces.loVect();
    const int *thi = tforces.hiVect();
    const int *glo = gp.loVect();
    const int *ghi = gp.hiVect();
    const int *rlo = rho.loVect();
    const int *rhi = rho.hiVect();
    
    const Real *UOdat = Uold.dataPtr();
    const Real *UNdat = Unew.dataPtr();
    const Real *AOdat = Aofs.dataPtr();
    const Real *TFdat = tforces.dataPtr();
    const Real *GPdat = gp.dataPtr();
    const Real *RHdat = rho.dataPtr();
    
    FORT_UPDATE_AOFS_TF_GP( UOdat, GDIMS(ulo,uhi),
                            UNdat, GDIMS(ulo,uhi),
                            AOdat, GDIMS(alo,ahi),
                            TFdat, GDIMS(tlo,thi),
                            GPdat, GDIMS(glo,ghi),
                            RHdat, GDIMS(rlo,rhi),
                            lo, hi, &dt );
}


// compute total source term for velocities,
// weighted by rho
//
// tforces = (tforces - gp)/rho
//
void Godunov::Sum_tf_gp( FArrayBox &tforces, 
                         FArrayBox &gp,      FArrayBox &rho )
{
    assert( rho.nComp()     == 1 );
    assert( tforces.nComp() >= BL_SPACEDIM );
    assert( gp.nComp()      == BL_SPACEDIM );
    
    const int *tlo = tforces.loVect();
    const int *thi = tforces.hiVect();
    const int *glo = gp.loVect();
    const int *ghi = gp.hiVect();
    const int *rlo = rho.loVect();
    const int *rhi = rho.hiVect();
    
    const Real *TFdat = tforces.dataPtr();
    const Real *GPdat = gp.dataPtr();
    const Real *RHdat = rho.dataPtr();
     
    FORT_SUM_TF_GP( TFdat, GDIMS(tlo,thi),
                    GPdat, GDIMS(glo,ghi),
                    RHdat, GDIMS(rlo,rhi),
                    tlo, thi );
}


// compute total source term for velocities,
// weighted by rho
//
// tforces = (tforces + visc - gp)/rho
//
void Godunov::Sum_tf_gp_visc( FArrayBox &tforces, FArrayBox &visc, 
                              FArrayBox &gp,      FArrayBox &rho )
{
    assert( rho.nComp()     == 1 );
    assert( tforces.nComp() >= BL_SPACEDIM );
    assert( visc.nComp()    >= BL_SPACEDIM );
    assert( gp.nComp()      == BL_SPACEDIM );
    
    const int *vlo = visc.loVect();  
    const int *vhi = visc.hiVect();
    const int *tlo = tforces.loVect();
    const int *thi = tforces.hiVect();
    const int *glo = gp.loVect();
    const int *ghi = gp.hiVect();
    const int *rlo = rho.loVect();
    const int *rhi = rho.hiVect();
    
    const Real *TFdat = tforces.dataPtr();
    const Real *VIdat = visc.dataPtr();
    const Real *GPdat = gp.dataPtr();
    const Real *RHdat = rho.dataPtr();
     
    FORT_SUM_TF_GP_VISC( TFdat, GDIMS(tlo,thi),
                         VIdat, GDIMS(vlo,vhi),
                         GPdat, GDIMS(glo,ghi),
                         RHdat, GDIMS(rlo,rhi),
                         tlo, thi );
}



// compute total source term for scalars.  Note for compatibility
// The switch iconserv, determines the form of the total source term
//
// iconserv==1   => tforces = tforces - divU*S
//
// iconserv==0   => tforces = (tforces)/rho
//
void Godunov::Sum_tf_divu( FArrayBox &S, FArrayBox &tforces,
                           int s_ind, int num_comp,
                           FArrayBox &divu,    FArrayBox &rho,
                           int iconserv )
{
    assert( S.nComp()       >= s_ind+num_comp );
    assert( tforces.nComp() >= s_ind+num_comp );
    assert( divu.nComp()    == 1              );
    assert( rho.nComp()     == 1              );
    
    const int *slo = S.loVect();
    const int *shi = S.hiVect();
    const int *tlo = tforces.loVect();
    const int *thi = tforces.hiVect();
    const int *dlo = divu.loVect();
    const int *dhi = divu.hiVect();
    const int *rlo = rho.loVect();
    const int *rhi = rho.hiVect();
    
    const Real *Sdat  = S.dataPtr(s_ind);
    const Real *TFdat = tforces.dataPtr(s_ind);
    const Real *DUdat = divu.dataPtr();
    const Real *RHdat = rho.dataPtr();
     
    FORT_SUM_TF_DIVU( Sdat,  GDIMS(slo,shi),
                      TFdat, GDIMS(tlo,thi),
                      DUdat, GDIMS(dlo,dhi),
                      RHdat, GDIMS(rlo,rhi),
                      tlo, thi, &num_comp, &iconserv );
}



// compute total source term for scalars.  Note for compatibility
// The switch iconserv, determines the form of the total source term
//
// iconserv==1   => tforces = tforces + visc - divU*S
//
// iconserv==0   => tforces = (tforces+ visc)/rho
//
void Godunov::Sum_tf_divu_visc( FArrayBox &S, FArrayBox &tforces,
                                int s_ind, int num_comp,
                                FArrayBox &visc,
                                int v_ind,
                                FArrayBox &divu,    FArrayBox &rho,
                                int iconserv )
{
    assert( S.nComp()       >= s_ind+num_comp );
    assert( tforces.nComp() >= s_ind+num_comp );
    assert( divu.nComp()    == 1              );
    assert( visc.nComp()    >= v_ind+num_comp );
    assert( rho.nComp()     == 1              );
    
    const int *slo = S.loVect();
    const int *shi = S.hiVect();
    const int *tlo = tforces.loVect();
    const int *thi = tforces.hiVect();
    const int *dlo = divu.loVect();
    const int *dhi = divu.hiVect();
    const int *vlo = visc.loVect();
    const int *vhi = visc.hiVect();
    const int *rlo = rho.loVect();
    const int *rhi = rho.hiVect();
    
    const Real *Sdat  = S.dataPtr(s_ind);
    const Real *TFdat = tforces.dataPtr(s_ind);
    const Real *DUdat = divu.dataPtr();
    const Real *VIdat = visc.dataPtr(v_ind);
    const Real *RHdat = rho.dataPtr();
     
    FORT_SUM_TF_DIVU_VISC( Sdat,  GDIMS(slo,shi),
                           TFdat, GDIMS(tlo,thi),
                           DUdat, GDIMS(dlo,dhi),
                           VIdat, GDIMS(vlo,vhi),
                           RHdat, GDIMS(rlo,rhi),
                           tlo, thi, &num_comp, &iconserv );
}

