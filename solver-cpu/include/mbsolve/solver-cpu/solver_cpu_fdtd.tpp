/*
 * mbsolve: An open-source solver tool for the Maxwell-Bloch equations.
 *
 * Copyright (c) 2016, Computational Photonics Group, Technical University of
 * Munich.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301  USA
 */

#ifndef MBSOLVE_SOLVER_CPU_FDTD_TPP
#define MBSOLVE_SOLVER_CPU_FDTD_TPP

#define EIGEN_DONT_PARALLELIZE
#define EIGEN_NO_MALLOC
#define EIGEN_STRONG_INLINE inline

#include <iostream>
#include <omp.h>
#include <mbsolve/solver-cpu/internal/common_cpu.hpp>
#include <mbsolve/solver-cpu/solver_cpu_fdtd.hpp>

namespace mbsolve {

template<unsigned int num_lvl, template<unsigned int> class density_algo>
solver_cpu_fdtd<num_lvl, density_algo>::solver_cpu_fdtd(
    std::shared_ptr<const device> dev,
    std::shared_ptr<scenario> scen)
  : solver(
        "cpu-fdtd-" + std::to_string(num_lvl) + "lvl-" +
            density_algo<num_lvl>::name(),
        dev,
        scen)
{
    /* TODO: scenario, device sanity check */
    /*
     * device.length > 0 (-> regions.size() > 0)
     * required materials found?
     * no gap in regions
     *
     */

    /* TODO: solver params
     * courant number
     */

    /* report number of used OpenMP threads */
    unsigned int P = omp_get_max_threads();
    std::cout << "Number of threads: " << P << std::endl;

    if (dev->get_regions().size() == 0) {
        throw std::invalid_argument("No regions in device!");
    }

    /* determine simulation settings */
    init_fdtd_simulation(dev, scen, 0.5);

    /* inverse grid point size for Ampere's law */
    m_dx_inv = 1.0 / scen->get_gridpoint_size();

    /* set up simulation constants for quantum mechanical description */
    std::vector<sim_constants_fdtd> m_sim_consts_fdtd;
    std::map<std::string, unsigned int> id_to_idx;
    unsigned int j = 0;
    for (const auto& mat_id : dev->get_used_materials()) {
        auto mat = material::get_from_library(mat_id);

        /* constants for electromagnetic (optical) field */
        m_sim_consts_fdtd.push_back(get_fdtd_constants(dev, scen, mat));

        /* constants for quantum mechanical system */
        m_sim_consts_qm.push_back(density_algo<num_lvl>::get_qm_constants(
            mat->get_qm(), scen->get_timestep_size()));

        /* enter material index */
        id_to_idx[mat->get_id()] = j;
        j++;
    }

    /* allocate data arrays */
    typedef typename density_algo<num_lvl>::density density_t;
    m_d = new density_t[scen->get_num_gridpoints()];

    m_h = (real*) mb_aligned_alloc(
        sizeof(real) * (scen->get_num_gridpoints() + 1));
    m_e = (real*) mb_aligned_alloc(sizeof(real) * scen->get_num_gridpoints());
    m_p = (real*) mb_aligned_alloc(sizeof(real) * scen->get_num_gridpoints());

    m_fac_a =
        (real*) mb_aligned_alloc(sizeof(real) * scen->get_num_gridpoints());
    m_fac_b =
        (real*) mb_aligned_alloc(sizeof(real) * scen->get_num_gridpoints());
    m_fac_c =
        (real*) mb_aligned_alloc(sizeof(real) * scen->get_num_gridpoints());
    m_gamma =
        (real*) mb_aligned_alloc(sizeof(real) * scen->get_num_gridpoints());

    m_mat_indices = (unsigned int*) mb_aligned_alloc(
        sizeof(unsigned int) * scen->get_num_gridpoints());

    /* set up indices array and initialize data arrays */
#pragma omp parallel for schedule(static)
    for (int i = 0; i < scen->get_num_gridpoints(); i++) {
        /* determine index of material and whether it has qm description */
        int idx = -1;
        bool has_qm = false;
        real x = i * scen->get_gridpoint_size();
        for (const auto& reg : dev->get_regions()) {
            if ((x >= reg->get_x_start()) && (x <= reg->get_x_end())) {
                idx = id_to_idx[reg->get_material()->get_id()];
                has_qm = (reg->get_material()->get_qm()) ? true : false;
                break;
            }
        }
        /* TODO: assert/bug if idx == -1 */
        if ((idx < 0) || (idx >= dev->get_used_materials().size())) {
            std::cout << "At index " << i << std::endl;
            throw std::invalid_argument("region not found");
        }

        /* material parameters */
        m_fac_a[i] = m_sim_consts_fdtd[idx].fac_a;
        m_fac_b[i] = m_sim_consts_fdtd[idx].fac_b;
        m_fac_c[i] = m_sim_consts_fdtd[idx].fac_c;
        m_gamma[i] = m_sim_consts_fdtd[idx].gamma;
        m_mat_indices[i] = idx;

        /* initialization */
        if (has_qm) {
            auto ic_dm = scen->get_ic_density();
            m_d[i] = density_algo<num_lvl>::get_density(ic_dm->initialize(x));
        } else {
            m_d[i] = density_algo<num_lvl>::get_density();
        }
        auto ic_e = scen->get_ic_electric();
        auto ic_h = scen->get_ic_magnetic();
        m_e[i] = ic_e->initialize(x);
        m_h[i] = ic_h->initialize(x);
        if (i == scen->get_num_gridpoints() - 1) {
            m_h[i + 1] = 0.0;
        }
        m_p[i] = 0.0;
    }

    /* set up results and transfer data structures */
    unsigned int scratch_size = 0;
    for (const auto& rec : scen->get_records()) {
        /* create copy list entry */
        copy_list_entry entry(rec, scen, scratch_size);

        /* add result to solver */
        m_results.push_back(entry.get_result());

        /* calculate scratch size */
        scratch_size += entry.get_size();

        /* take imaginary part into account */
        if (rec->is_complex()) {
            scratch_size += entry.get_size();
        }

        /* TODO check if result is available */
        /*
          throw std::invalid_argument("Requested result is not available!");
        */

        m_copy_list.push_back(entry);
    }

    /* allocate scratchpad result memory */
    m_result_scratch = (real*) mb_aligned_alloc(sizeof(real) * scratch_size);

    /* create source data */
    m_source_data =
        new real[scen->get_num_timesteps() * scen->get_sources().size()];
    unsigned int base_idx = 0;
    for (const auto& src : scen->get_sources()) {
        sim_source s;
        s.type = src->get_type();
        s.x_idx = src->get_position() / scen->get_gridpoint_size();
        s.data_base_idx = base_idx;
        m_sim_sources.push_back(s);

        /* calculate source values */
        for (unsigned int j = 0; j < scen->get_num_timesteps(); j++) {
            m_source_data[base_idx + j] =
                src->get_value(j * scen->get_timestep_size());
        }

        base_idx += scen->get_num_timesteps();
    }
}

template<unsigned int num_lvl, template<unsigned int> class density_algo>
solver_cpu_fdtd<num_lvl, density_algo>::~solver_cpu_fdtd()
{
    mb_aligned_free(m_fac_a);
    mb_aligned_free(m_fac_b);
    mb_aligned_free(m_fac_c);
    mb_aligned_free(m_gamma);

    mb_aligned_free(m_h);
    mb_aligned_free(m_e);
    mb_aligned_free(m_p);

    mb_aligned_free(m_mat_indices);
    mb_aligned_free(m_result_scratch);
    delete[] m_source_data;
    delete[] m_d;
}

template<unsigned int num_lvl, template<unsigned int> class density_algo>
void
solver_cpu_fdtd<num_lvl, density_algo>::run() const
{
#pragma omp parallel
    {
        /* main loop */
        for (int n = 0; n < m_scenario->get_num_timesteps(); n++) {

            /* update electric field in parallel */
#if USE_OMP_SIMD
#pragma omp for simd schedule(static)
#else
#pragma omp for schedule(static)
#endif
            for (int i = 0; i < m_scenario->get_num_gridpoints(); i++) {
                m_e[i] = m_fac_a[i] * m_e[i] +
                    m_fac_b[i] *
                        (-m_gamma[i] * m_p[i] +
                         (m_h[i + 1] - m_h[i]) * m_dx_inv);
            }

            /* TODO only one thread should apply a certain source */
            /* apply sources */
            for (const auto& src : m_sim_sources) {
                /* TODO: support other source types than hard sources */
                if (src.type == source::type::hard_source) {
                    m_e[src.x_idx] = m_source_data[src.data_base_idx + n];
                } else if (src.type == source::type::soft_source) {
                    m_e[src.x_idx] += m_source_data[src.data_base_idx + n];
                } else {
                }
            }

            /* update magnetic field in parallel */
#if USE_OMP_SIMD
#pragma omp for simd nowait schedule(static)
#else
#pragma omp for nowait schedule(static)
#endif
            for (int i = 1; i < m_scenario->get_num_gridpoints(); i++) {
                m_h[i] += m_fac_c[i] * (m_e[i] - m_e[i - 1]);
            }
            /* no implicit synchronization required */

            /* TODO PMC BC could be implemented intrinsically, since m_h[0]
             * and m_h[end] are never updated.
             * However, let us see how the new flexible BC code is
             * implemented.
             */
            /* apply boundary condition */
            m_h[0] = 0;
            m_h[m_scenario->get_num_gridpoints()] = 0;

            /* update density matrix in parallel */
#pragma omp for schedule(static)
            for (int i = 0; i < m_scenario->get_num_gridpoints(); i++) {
                unsigned int mat_idx = m_mat_indices[i];

                /* update density matrix */
                density_algo<num_lvl>::update(
                    m_sim_consts_qm[mat_idx], m_d[i], m_e[i], &m_p[i]);
            }

            /* save results to scratchpad in parallel */
            for (const auto& cle : m_copy_list) {
                if (cle.hasto_record(n)) {
                    uint64_t pos = cle.get_position();
                    uint64_t cols = cle.get_cols();
                    record::type t = cle.get_type();
                    uint64_t o_r = cle.get_offset_scratch_real(n, 0);
                    unsigned int cidx = cle.get_col_idx();
                    unsigned int ridx = cle.get_row_idx();

//#pragma omp for schedule(static)
                    //for (int i = pos; i < pos + cols; i++) {
                    //EDIT
                    //Only the first point on x is saved
                    if (t == record::type::electric) {
                        m_result_scratch[o_r - pos] = m_e[0];
                    } else if (t == record::type::polar_dt) {
                        m_result_scratch[o_r - pos] = m_p[0];
                    } else if (t == record::type::magnetic) {
                        m_result_scratch[o_r - pos] = m_h[0];
                    } else if (t == record::type::inversion) {
                        m_result_scratch[o_r - pos] =
                            density_algo<num_lvl>::calc_inversion(m_d[0]);
                    } else if (t == record::type::density) {
                        /* right now only populations */
                        if (ridx == cidx) {
                            m_result_scratch[o_r - pos] =
                                density_algo<num_lvl>::calc_population(
                                    m_d[0], ridx);
                        } else {
                            /* coherence terms */

                            /* TODO */
                        }
                    } else {
                        /* TODO handle trouble */
                    }
                    //}
                    /* TODO handle imaginary */
                }
            }
        }
    }

    /* bulk copy results into result classes */
    for (const auto& cle : m_copy_list) {
        real* dr = m_result_scratch + cle.get_offset_scratch_real(0, 0);
        std::copy(dr, dr + cle.get_size(), cle.get_result_real(0, 0));
        if (cle.is_complex()) {
            real* di = m_result_scratch + cle.get_offset_scratch_imag(0, 0);
            std::copy(di, di + cle.get_size(), cle.get_result_imag(0, 0));
        }
    }
}
}

#endif
