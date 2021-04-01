#include <iamr_godunov.H>
#include <iamr_advection.H>
#ifdef AMREX_USE_EB
#include <iamr_ebgodunov.H>
#include <iamr_redistribution.H>
#endif

using namespace amrex;

void
Godunov::ComputeAofs ( MultiFab& aofs, const int aofs_comp, const int ncomp,
                       MultiFab const& state, const int state_comp,
                       AMREX_D_DECL( MultiFab const& umac,
                                     MultiFab const& vmac,
                                     MultiFab const& wmac),
                       AMREX_D_DECL( MultiFab& xedge,
                                     MultiFab& yedge,
                                     MultiFab& zedge),
                       const int  edge_comp,
                       const bool known_edgestate,
                       AMREX_D_DECL( MultiFab& xfluxes,
                                     MultiFab& yfluxes,
                                     MultiFab& zfluxes),
                       int fluxes_comp,
                       MultiFab const& fq,
                       const int fq_comp,
                       MultiFab const& divu,
                       Vector<BCRec> const& h_bc,
                       BCRec const* d_bc,
                       Geometry const& geom,
                       Gpu::DeviceVector<int>& iconserv,
                       const Real dt,
                       const bool is_velocity,
                       const bool use_ppm,
                       const bool use_forces_in_trans,
                       std::string redistribution_type)
{
    BL_PROFILE("Godunov::ComputeAofs()");

#ifdef AMREX_USE_EB
    AMREX_ALWAYS_ASSERT(state.hasEBFabFactory());

    for (int n = 0; n < ncomp; n++)
       if (!iconserv[n]) amrex::Abort("Non-conservative form of Godunov is not supported for EB geometries");

    if (use_ppm)
        amrex::Abort("Godunov does not support use_ppm=true for EB geometries");
    if (use_forces_in_trans)
        amrex::Abort("Godunov does not support use_forces_in_trans=true for EB geometries");

    auto const& ebfact= dynamic_cast<EBFArrayBoxFactory const&>(state.Factory());
    auto const& flags = ebfact.getMultiEBCellFlagFab();
    auto const& fcent = ebfact.getFaceCent();
    auto const& ccent = ebfact.getCentroid();
    auto const& vfrac = ebfact.getVolFrac();
    auto const& areafrac = ebfact.getAreaFrac();
#endif

    //FIXME - check on adding tiling here
    for (MFIter mfi(aofs); mfi.isValid(); ++mfi)
    {

        const Box& bx   = mfi.tilebox();
#ifdef AMREX_USE_EB
        auto const& flagfab = ebfact.getMultiEBCellFlagFab()[mfi];
        bool regular = (flagfab.getType(amrex::grow(bx,2)) == FabType::regular);
#else
        bool regular = true;
#endif
        //
        // Get handlers to Array4
        //
        AMREX_D_TERM( const auto& fx = xfluxes.array(mfi,fluxes_comp);,
                      const auto& fy = yfluxes.array(mfi,fluxes_comp);,
                      const auto& fz = zfluxes.array(mfi,fluxes_comp););

        AMREX_D_TERM( const auto& xed = xedge.array(mfi,edge_comp);,
                      const auto& yed = yedge.array(mfi,edge_comp);,
                      const auto& zed = zedge.array(mfi,edge_comp););

        AMREX_D_TERM( const auto& u = umac.const_array(mfi);,
                      const auto& v = vmac.const_array(mfi);,
                      const auto& w = wmac.const_array(mfi););

        if (regular)   // Plain Godunov
        {
            if (not known_edgestate)
            {
                Godunov::ComputeEdgeState( bx, ncomp,
                                           state.array(mfi,state_comp),
                                           AMREX_D_DECL( xed, yed, zed ),
                                           AMREX_D_DECL( u, v, w ),
                                           divu.array(mfi),
                                           fq.array(mfi,fq_comp),
                                           geom, dt, d_bc,
                                           iconserv.data(),
                                           use_ppm,
                                           use_forces_in_trans,
                                           is_velocity );
            }

            Advection::ComputeFluxes( bx, AMREX_D_DECL( fx, fy, fz ),
                                      AMREX_D_DECL( u, v, w ),
                                      AMREX_D_DECL( xed, yed, zed ),
                                      geom, ncomp );

            Advection::ComputeDivergence( bx,
                                          aofs.array(mfi,aofs_comp),
                                          AMREX_D_DECL( fx, fy, fz ),
                                          AMREX_D_DECL( xed, yed, zed ),
                                          AMREX_D_DECL( u, v, w ),
                                          ncomp, geom, iconserv.data() );
        }
#ifdef AMREX_USE_EB
        else     // EB Godunov
        {


            Box gbx = bx;
            // We need 3 if we are doing state redistribution
            if (redistribution_type == "StateRedist" ||
                redistribution_type == "MergeRedist")
                gbx.grow(3);
            else if (redistribution_type == "FluxRedist")
                gbx.grow(2);
            else if (redistribution_type == "NoRedist")
                gbx.grow(1);
            else
                amrex::Abort("Dont know this redistribution type");

            AMREX_D_TERM(Array4<Real const> const& fcx = fcent[0]->const_array(mfi);,
                         Array4<Real const> const& fcy = fcent[1]->const_array(mfi);,
                         Array4<Real const> const& fcz = fcent[2]->const_array(mfi););

            AMREX_D_TERM(Array4<Real const> const& apx = areafrac[0]->const_array(mfi);,
                         Array4<Real const> const& apy = areafrac[1]->const_array(mfi);,
                         Array4<Real const> const& apz = areafrac[2]->const_array(mfi););

            Array4<Real const> const& ccent_arr = ccent.const_array(mfi);
            Array4<Real const> const& vfrac_arr = vfrac.const_array(mfi);
            auto const& flags_arr  = flags.const_array(mfi);

            int ngrow = 4;

            if (redistribution_type=="StateRedist")
                ++ngrow;

            FArrayBox tmpfab(amrex::grow(bx,ngrow),  (4*AMREX_SPACEDIM + 2)*ncomp);
            Elixir    eli = tmpfab.elixir();


            if (not known_edgestate)
            {
                EBGodunov::ComputeEdgeState( gbx, ncomp,
                                             state.array(mfi,state_comp),
                                             AMREX_D_DECL( xed, yed, zed ),
                                             AMREX_D_DECL( u, v, w ),
                                             divu.array(mfi),
                                             fq.array(mfi,fq_comp),
                                             geom, dt, h_bc, d_bc,
                                             iconserv.data(),
                                             tmpfab.dataPtr(),
                                             flags_arr,
                                             AMREX_D_DECL( apx, apy, apz ),
                                             vfrac_arr,
                                             AMREX_D_DECL( fcx, fcy, fcz ),
                                             ccent_arr,
                                             is_velocity );
            }

            Advection::EB_ComputeFluxes( gbx, AMREX_D_DECL( fx, fy, fz ),
                                         AMREX_D_DECL( u, v, w ),
                                         AMREX_D_DECL( xed, yed, zed ),
                                         AMREX_D_DECL( apx, apy, apz ),
                                         geom, ncomp, flags_arr );

            // div at ncomp*3 to make space for the 3 redistribute temporaries
            Array4<Real> divtmp_arr = tmpfab.array(ncomp*3);

            Advection::EB_ComputeDivergence( gbx,
                                             divtmp_arr,
                                             AMREX_D_DECL( fx, fy, fz ),
                                             vfrac_arr, ncomp, geom );

            Array4<Real> scratch = tmpfab.array(0);
            Redistribution::Apply( bx, ncomp, aofs.array(mfi, aofs_comp), divtmp_arr,
                                   state.const_array(mfi, state_comp), scratch, flags_arr,
                                   AMREX_D_DECL(apx,apy,apz), vfrac_arr,
                                   AMREX_D_DECL(fcx,fcy,fcz), ccent_arr, geom, dt,
                                   redistribution_type );

            // Change sign because for EB we computed -div
            aofs[mfi].mult(-1., bx, aofs_comp, ncomp);

        }
#endif

        // Note this sync is needed since ComputeEdgeState() contains temporaries
	// Not sure it's really needed when known_edgestate==true
        Gpu::streamSynchronize();  // otherwise we might be using too much memory
    }


}




void
Godunov::ComputeSyncAofs ( MultiFab& aofs, const int aofs_comp, const int ncomp,
                           MultiFab const& state, const int state_comp,
                           AMREX_D_DECL( MultiFab const& umac,
                                         MultiFab const& vmac,
                                         MultiFab const& wmac),
                           AMREX_D_DECL( MultiFab const& ucorr,
                                         MultiFab const& vcorr,
                                         MultiFab const& wcorr),
                           AMREX_D_DECL( MultiFab& xedge,
                                         MultiFab& yedge,
                                         MultiFab& zedge),
                           const int  edge_comp,
                           const bool known_edgestate,
                           AMREX_D_DECL( MultiFab& xfluxes,
                                         MultiFab& yfluxes,
                                         MultiFab& zfluxes),
                           int fluxes_comp,
                           MultiFab const& fq,
                           const int fq_comp,
                           MultiFab const& divu,
                           Vector<BCRec> const& h_bc,
                           BCRec const* d_bc,
                           Geometry const& geom,
                           Gpu::DeviceVector<int>& iconserv,
                           const Real dt,
                           const bool is_velocity,
                           const bool use_ppm,
                           const bool use_forces_in_trans,
                           std::string redistribution_type )
{
    BL_PROFILE("Godunov::ComputeAofs()");

#ifdef AMREX_USE_EB
    AMREX_ALWAYS_ASSERT(state.hasEBFabFactory());

    for (int n = 0; n < ncomp; n++)
       if (!iconserv[n]) amrex::Abort("Non-conservative form of Godunov is not supported for EB geometries");

    if (use_ppm)
        amrex::Abort("Godunov does not support use_ppm=true for EB geometries");
    if (use_forces_in_trans)
        amrex::Abort("Godunov does not support use_forces_in_trans=true for EB geometries");


    auto const& ebfact= dynamic_cast<EBFArrayBoxFactory const&>(state.Factory());
    auto const& flags = ebfact.getMultiEBCellFlagFab();
    auto const& fcent = ebfact.getFaceCent();
    auto const& ccent = ebfact.getCentroid();
    auto const& vfrac = ebfact.getVolFrac();
    auto const& areafrac = ebfact.getAreaFrac();
#endif

    // Sync divergence computation is always conservative
    std::vector<int> div_iconserv(ncomp,1);

    //FIXME - check on adding tiling here
    for (MFIter mfi(aofs); mfi.isValid(); ++mfi)
    {

        const Box& bx   = mfi.tilebox();

#ifdef AMREX_USE_EB
        auto const& flagfab = ebfact.getMultiEBCellFlagFab()[mfi];
        bool regular = (flagfab.getType(amrex::grow(bx,2)) == FabType::regular);
#else
        bool regular = true;
#endif

        //
        // Get handlers to Array4
        //
        AMREX_D_TERM( const auto& fx = xfluxes.array(mfi,fluxes_comp);,
                      const auto& fy = yfluxes.array(mfi,fluxes_comp);,
                      const auto& fz = zfluxes.array(mfi,fluxes_comp););

        AMREX_D_TERM( const auto& xed = xedge.array(mfi,edge_comp);,
                      const auto& yed = yedge.array(mfi,edge_comp);,
                      const auto& zed = zedge.array(mfi,edge_comp););

        AMREX_D_TERM( const auto& uc = ucorr.const_array(mfi);,
                      const auto& vc = vcorr.const_array(mfi);,
                      const auto& wc = wcorr.const_array(mfi););

        if (regular)  // Plain Godunov
        {
            if (not known_edgestate)
            {

                AMREX_D_TERM( const auto& u = umac.const_array(mfi);,
                              const auto& v = vmac.const_array(mfi);,
                              const auto& w = wmac.const_array(mfi););

                ComputeEdgeState( bx, ncomp,
                                  state.array(mfi,state_comp),
                                  AMREX_D_DECL( xed, yed, zed ),
                                  AMREX_D_DECL( u, v, w ),
                                  divu.array(mfi),
                                  fq.array(mfi,fq_comp),
                                  geom, dt, d_bc,
                                  iconserv.data(),
                                  use_ppm,
                                  use_forces_in_trans,
                                  is_velocity );
            }

            Advection::ComputeFluxes( bx, AMREX_D_DECL( fx, fy, fz ),
                                      AMREX_D_DECL( uc, vc, wc ),
                                      AMREX_D_DECL( xed, yed, zed ),
                                      geom, ncomp );


            Advection::ComputeDivergence( bx, aofs.array(mfi, aofs_comp), D_DECL(fx,fy,fz),
                                          D_DECL( xed, yed, zed ), D_DECL( uc, vc, wc ),
                                          ncomp, geom, div_iconserv.data());
        }
#ifdef AMREX_USE_EB
        else     // EB Godunov
        {

            Box gbx = bx;
            gbx.grow(2);

            AMREX_D_TERM(Array4<Real const> const& fcx = fcent[0]->const_array(mfi);,
                         Array4<Real const> const& fcy = fcent[1]->const_array(mfi);,
                         Array4<Real const> const& fcz = fcent[2]->const_array(mfi););

            AMREX_D_TERM(Array4<Real const> const& apx = areafrac[0]->const_array(mfi);,
                         Array4<Real const> const& apy = areafrac[1]->const_array(mfi);,
                         Array4<Real const> const& apz = areafrac[2]->const_array(mfi););

            Array4<Real const> const& ccent_arr = ccent.const_array(mfi);
            Array4<Real const> const& vfrac_arr = vfrac.const_array(mfi);
            auto const& flags_arr  = flags.const_array(mfi);

            int ngrow = 4;
            FArrayBox tmpfab(amrex::grow(bx,ngrow),  (4*AMREX_SPACEDIM + 2)*ncomp);
            Elixir    eli = tmpfab.elixir();


            if (not known_edgestate)
            {
                AMREX_D_TERM( const auto& u = umac.const_array(mfi);,
                              const auto& v = vmac.const_array(mfi);,
                              const auto& w = wmac.const_array(mfi););

                EBGodunov::ComputeEdgeState( gbx, ncomp,
                                             state.array(mfi,state_comp),
                                             AMREX_D_DECL( xed, yed, zed ),
                                             AMREX_D_DECL( u, v, w ),
                                             divu.array(mfi),
                                             fq.array(mfi,fq_comp),
                                             geom, dt, h_bc, d_bc,
                                             iconserv.data(),
                                             tmpfab.dataPtr(),
                                             flags_arr,
                                             AMREX_D_DECL( apx, apy, apz ),
                                             vfrac_arr,
                                             AMREX_D_DECL( fcx, fcy, fcz ),
                                             ccent_arr,
                                             is_velocity );
            }

            Advection::EB_ComputeFluxes( gbx, AMREX_D_DECL( fx, fy, fz ),
                                         AMREX_D_DECL( uc, vc, wc ),
                                         AMREX_D_DECL( xed, yed, zed ),
                                         AMREX_D_DECL( apx, apy, apz ),
                                         geom, ncomp, flags_arr );

            // div at ncomp*3 to make space for the 3 redistribute temporaries
            Array4<Real> divtmp_arr = tmpfab.array(ncomp*3);
            Array4<Real> divtmp_redist_arr = tmpfab.array(ncomp*4);

            Advection::EB_ComputeDivergence( gbx,
                                             divtmp_arr,
                                             AMREX_D_DECL( fx, fy, fz ),
                                             vfrac_arr, ncomp, geom );

            Array4<Real> scratch = tmpfab.array(0);

            Redistribution::Apply( bx, ncomp, divtmp_redist_arr, divtmp_arr,
                                   state.const_array(mfi, state_comp), scratch, flags_arr,
                                   AMREX_D_DECL(apx,apy,apz), vfrac_arr,
                                   AMREX_D_DECL(fcx,fcy,fcz), ccent_arr, geom, dt,
                                   redistribution_type );

            // Subtract contribution to sync aofs -- sign of divergence is aofs is opposite
            // of sign to div as computed by EBGOdunov::ComputeDivergence, thus it must be subtracted.
            auto const& aofs_arr = aofs.array(mfi, aofs_comp);
            amrex::ParallelFor(bx, ncomp, [aofs_arr, divtmp_redist_arr]
            AMREX_GPU_DEVICE (int i, int j, int k, int n) noexcept
            {
                aofs_arr( i, j, k, n ) += -divtmp_redist_arr( i, j, k, n );
            });

        }
#endif

	// Note this sync is needed since ComputeEdgeState() contains temporaries
	// Not sure it's really needed when known_edgestate==true
        Gpu::streamSynchronize();  // otherwise we might be using too much memory
    }
}
