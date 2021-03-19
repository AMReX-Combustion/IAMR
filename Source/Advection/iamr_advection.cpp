#include <iamr_advection.H>

using namespace amrex;


void
Advection::ComputeFluxes ( Box const& bx,
                           AMREX_D_DECL( Array4<Real> const& fx,
                                         Array4<Real> const& fy,
                                         Array4<Real> const& fz),
                           AMREX_D_DECL( Array4<Real const> const& umac,
                                         Array4<Real const> const& vmac,
                                         Array4<Real const> const& wmac),
                           AMREX_D_DECL( Array4<Real const> const& xed,
                                         Array4<Real const> const& yed,
                                         Array4<Real const> const& zed),
                           Geometry const& geom, const int ncomp )
{

    const auto dx = geom.CellSizeArray();

    GpuArray<Real,AMREX_SPACEDIM> area;
#if ( AMREX_SPACEDIM == 3 )
    area[0] = dx[1]*dx[2];
    area[1] = dx[0]*dx[2];
    area[2] = dx[0]*dx[1];
#else
    area[0] = dx[1];
    area[1] = dx[0];
#endif

    //
    //  X flux
    //
    const Box& xbx = amrex::surroundingNodes(bx,0);

    amrex::ParallelFor(xbx, ncomp, [fx, umac, xed, area]
    AMREX_GPU_DEVICE (int i, int j, int k, int n) noexcept
    {
        fx(i,j,k,n) = xed(i,j,k,n) * umac(i,j,k) * area[0];
    });

    //
    //  y flux
    //
    const Box& ybx = amrex::surroundingNodes(bx,1);

    amrex::ParallelFor(ybx, ncomp, [fy, vmac, yed, area]
    AMREX_GPU_DEVICE (int i, int j, int k, int n) noexcept
    {
        fy(i,j,k,n) = yed(i,j,k,n) * vmac(i,j,k) * area[1];
    });

#if (AMREX_SPACEDIM==3)
    //
    //  z flux
    //
    const Box& zbx = amrex::surroundingNodes(bx,2);

    amrex::ParallelFor(zbx, ncomp, [fz, wmac, zed, area]
    AMREX_GPU_DEVICE (int i, int j, int k, int n) noexcept
    {
        fz(i,j,k,n) = zed(i,j,k,n) * wmac(i,j,k) * area[2];
    });

#endif

}



void
Advection::ComputeDivergence ( Box const& bx,
                             Array4<Real> const& div,
                             AMREX_D_DECL( Array4<Real const> const& fx,
                                           Array4<Real const> const& fy,
                                           Array4<Real const> const& fz),
                             AMREX_D_DECL( Array4<Real const> const& xed,
                                           Array4<Real const> const& yed,
                                           Array4<Real const> const& zed),
                             AMREX_D_DECL( Array4<Real const> const& umac,
                                           Array4<Real const> const& vmac,
                                           Array4<Real const> const& wmac),
                             const int ncomp, Geometry const& geom,
                             int const* iconserv )
{

    const auto dxinv = geom.InvCellSizeArray();

#if (AMREX_SPACEDIM==3)
    Real qvol = dxinv[0] * dxinv[1] * dxinv[2];
#else
    Real qvol = dxinv[0] * dxinv[1];
#endif

    amrex::ParallelFor(bx, ncomp,[=]
    AMREX_GPU_DEVICE (int i, int j, int k, int n) noexcept
    {
        if (iconserv[n])
        {
            div(i,j,k,n) =  qvol *
                (
                         fx(i+1,j,k,n) -  fx(i,j,k,n)
                       + fy(i,j+1,k,n) -  fy(i,j,k,n)
#if (AMREX_SPACEDIM==3)
                       + fz(i,j,k+1,n) -  fz(i,j,k,n)
#endif
                );
        }
        else
        {
	    div(i,j,k,n) = 0.5*dxinv[0]*( umac(i+1,j,k  ) +  umac(i,j,k  ))
                *                       (  xed(i+1,j,k,n) -   xed(i,j,k,n))
                +          0.5*dxinv[1]*( vmac(i,j+1,k  ) +  vmac(i,j,k  ))
                *                       (  yed(i,j+1,k,n) -   yed(i,j,k,n))
#if (AMREX_SPACEDIM==3)
                +          0.5*dxinv[2]*( wmac(i,j,k+1  ) +  wmac(i,j,k  ))
                *                       (  zed(i,j,k+1,n) -   zed(i,j,k,n))
#endif
                ;
       }

    });
}


void
Advection::ComputeSyncDivergence ( Box const& bx,
                                   Array4<Real> const& div,
                                   AMREX_D_DECL( Array4<Real const> const& fx,
                                                 Array4<Real const> const& fy,
                                                 Array4<Real const> const& fz),
                                   const int ncomp, Geometry const& geom )
{

    const auto dxinv = geom.InvCellSizeArray();

#if (AMREX_SPACEDIM==3)
    Real qvol = dxinv[0] * dxinv[1] * dxinv[2];
#else
    Real qvol = dxinv[0] * dxinv[1];
#endif

    amrex::ParallelFor(bx, ncomp,[=]
    AMREX_GPU_DEVICE (int i, int j, int k, int n) noexcept
    {
        div(i,j,k,n) +=  qvol * (
              fx(i+1,j,k,n) -  fx(i,j,k,n)
            + fy(i,j+1,k,n) -  fy(i,j,k,n)
#if (AMREX_SPACEDIM==3)
            + fz(i,j,k+1,n) -  fz(i,j,k,n)
#endif
            );
    });
}


///////////////////////////////////////////////////////////////////////////
//                                                                       //
//   EB routines                                                         //
//                                                                       //
///////////////////////////////////////////////////////////////////////////
#ifdef AMREX_USE_EB

void
Advection::EB_ComputeDivergence ( Box const& bx,
                                  Array4<Real> const& div,
                                  AMREX_D_DECL( Array4<Real const> const& fx,
                                                Array4<Real const> const& fy,
                                                Array4<Real const> const& fz),
                                  Array4<Real const> const& vfrac,
                                  const int ncomp, Geometry const& geom )
{

    const auto dxinv = geom.InvCellSizeArray();

#if (AMREX_SPACEDIM==3)
    Real qvol = dxinv[0] * dxinv[1] * dxinv[2];
#else
    Real qvol = dxinv[0] * dxinv[1];
#endif

    // Return -div because reinitialization algo operates on it
    // instead of operatin on div
    amrex::ParallelFor(bx, ncomp, [=]
    AMREX_GPU_DEVICE (int i, int j, int k, int n) noexcept
    {
        if ( vfrac(i,j,k) > 0.)
        {
            div(i,j,k,n) =  - qvol/vfrac(i,j,k) *
                (
                         fx(i+1,j,k,n) -  fx(i,j,k,n)
                       + fy(i,j+1,k,n) -  fy(i,j,k,n)
#if (AMREX_SPACEDIM==3)
                       + fz(i,j,k+1,n) -  fz(i,j,k,n)
#endif
                );
        }
        else
        {
            div(i,j,k,n) = 0.0;
        }

    });
}


void
Advection::EB_ComputeFluxes ( Box const& bx,
                              AMREX_D_DECL( Array4<Real> const& fx,
                                            Array4<Real> const& fy,
                                            Array4<Real> const& fz),
                              AMREX_D_DECL( Array4<Real const> const& umac,
                                            Array4<Real const> const& vmac,
                                            Array4<Real const> const& wmac),
                              AMREX_D_DECL( Array4<Real const> const& xed,
                                            Array4<Real const> const& yed,
                                            Array4<Real const> const& zed),
                              AMREX_D_DECL( Array4<Real const> const& apx,
                                            Array4<Real const> const& apy,
                                            Array4<Real const> const& apz),
                              Geometry const& geom, const int ncomp,
                              Array4<EBCellFlag const> const& flag)
{

    const auto dx = geom.CellSizeArray();

    GpuArray<Real,AMREX_SPACEDIM> area;

#if ( AMREX_SPACEDIM == 3 )
    area[0] = dx[1]*dx[2];
    area[1] = dx[0]*dx[2];
    area[2] = dx[0]*dx[1];
#else
    area[0] = dx[1];
    area[1] = dx[0];
#endif

    //
    //  X flux
    //
    const Box& xbx = amrex::surroundingNodes(bx,0);

    amrex::ParallelFor(xbx, ncomp, [fx, umac, xed, area, apx, flag]
    AMREX_GPU_DEVICE (int i, int j, int k, int n) noexcept
    {
        if (flag(i,j,k).isConnected(-1,0,0))
            fx(i,j,k,n) = xed(i,j,k,n) * umac(i,j,k) * apx(i,j,k) * area[0];
        else
            fx(i,j,k,n) = 0.;
    });

    //
    //  y flux
    //
    const Box& ybx = amrex::surroundingNodes(bx,1);

    amrex::ParallelFor(ybx, ncomp, [fy, vmac, yed, area, apy, flag]
    AMREX_GPU_DEVICE (int i, int j, int k, int n) noexcept
    {
        if (flag(i,j,k).isConnected(0,-1,0))
            fy(i,j,k,n) = yed(i,j,k,n) * vmac(i,j,k) * apy(i,j,k) * area[1];
        else
            fy(i,j,k,n) = 0.;
    });

#if (AMREX_SPACEDIM==3)
    //
    //  z flux
    //
    const Box& zbx = amrex::surroundingNodes(bx,2);

    amrex::ParallelFor(zbx, ncomp, [fz, wmac, zed, area, apz, flag]
    AMREX_GPU_DEVICE (int i, int j, int k, int n) noexcept
    {
        if (flag(i,j,k).isConnected(0,0,-1))
            fz(i,j,k,n) = zed(i,j,k,n) * wmac(i,j,k) * apz(i,j,k) * area[2];
        else
            fz(i,j,k,n) = 0.;
    });
#endif

}

#endif
