#include "interactions.hpp"

#include <cmath>

// debug only
#include <iostream>

#include "fft.hpp"
#include "force_grid.hpp"
#include "globals.hpp"
#include "vectors.hpp"

void LJInteraction::eval(const vector<Vec3d>& positions, vector<Vec3d>& forces, vector<double>& energies) const {
    Vec3d r_vec = positions[atom_i1_] - positions[atom_i2_];
    double r_sqr = r_vec.lensqr();
    double r6 = r_sqr * r_sqr * r_sqr;
    double term_a = es12_ / (r6*r6);
    double term_b = es6_ / r6;
    double e = term_a - term_b;
    Vec3d f = (12*term_a - 6*term_b) / r_sqr * r_vec;
    energies[atom_i1_] += e;
    energies[atom_i2_] += e;
    forces[atom_i1_] += f;
    forces[atom_i2_] -= f;
}

void MorseInteraction::eval(const vector<Vec3d>& positions, vector<Vec3d>& forces, vector<double>& energies) const {
    Vec3d r_vec = positions[atom_i1_] - positions[atom_i2_];
    double r = r_vec.len();
    double d_exp = exp(- a_ * (r - re_));
    double e = de_ * (pow(d_exp, 2) - 2 * d_exp + 1);
    double f_abs = 2 * de_ * a_ * (pow(d_exp, 2) - d_exp);
    Vec3d f = f_abs / r * r_vec;
    energies[atom_i1_] += e;
    energies[atom_i2_] += e;
    forces[atom_i1_] += f;
    forces[atom_i2_] -= f;
}

void CoulombInteraction::eval(const vector<Vec3d>& positions, vector<Vec3d>& forces, vector<double>& energies) const {
    Vec3d r_vec = positions[atom_i1_] - positions[atom_i2_];
    double r = r_vec.len();
    double e = qq_ / r;
    Vec3d f = qq_ / (r*r*r) * r_vec;
    energies[atom_i1_] += e;
    energies[atom_i2_] += e;
    forces[atom_i1_] += f;
    forces[atom_i2_] -= f;
}

ElectrostaticPotentialInteraction::ElectrostaticPotentialInteraction(const DataGrid<double>& e_potential, double tip_charge, double gaussian_width) {
    const Vec3i& n_grid = e_potential.getNGrid();
    const Vec3d& spacing = e_potential.getSpacing();
    const Vec3d& origin = e_potential.getOrigin();
    
    // Reserve storage space for the force
    DataGrid<Vec3d> force(n_grid.x, n_grid.y, n_grid.z, Vec3d(0.0));
    force.setSpacing(spacing);
    force.setOrigin(origin);
    
    // Create Gaussian charge distribution of the tip
    DataGrid<double> rho_tip(n_grid.x, n_grid.y, n_grid.z, 0.0);
    rho_tip.setSpacing(spacing);
    Vec3d position;
    double r_sqr;
    double gaussian_width_sqr = pow(gaussian_width, 2);
    double gaussian_norm_factor = 1.0/pow(gaussian_width*sqrt(2.0*PI), 3);
    // Find suitable cutoff distance for the Gaussian
    int n_min = min(min(n_grid.x, n_grid.y), n_grid.z);
    int cutoff_dist = n_min;
    for (int ix = 0; ix < n_min-1; ix++) {
        double x_sqr = pow(ix*spacing.x, 2);
        if (exp(-0.5*x_sqr/gaussian_width_sqr) < 1.0e-10) { //TODO: make sure 1.0e-10 is a good cutoff value and define in globals
            cutoff_dist = ix;
            break;
        }
    }
    // Evaluate the Gaussian at grid points within the cutoff
    for (int ix = -cutoff_dist; ix <= cutoff_dist; ix++) {
        for (int iy = -cutoff_dist; iy <= cutoff_dist; iy++) {
            for (int iz = -cutoff_dist; iz < cutoff_dist; iz++) {
                position = rho_tip.positionAt(ix, iy, iz);
                r_sqr = position.lensqr();
                rho_tip.atPBC(ix, iy, iz) = tip_charge * gaussian_norm_factor * exp(-0.5*r_sqr/gaussian_width_sqr);
            }
        }
    }
    
    if (DEBUG_MODE) {
        double total_charge = 0.0;
        for (int ix = 0; ix < n_grid.x; ix++) {
            for (int iy = 0; iy < n_grid.y; iy++) {
                for (int iz = 0; iz < n_grid.z; iz++) {
                    total_charge += rho_tip.at(ix, iy, iz);
                }
            }
        }
        total_charge *= spacing.x*spacing.y*spacing.z;
        cout << "Total charge at tip: " << total_charge << endl;
    }
    
    // Do the FFTs for the potential and the charge distribution
    DataGrid<dcomplex> pot_kspace, rho_kspace;
    fft_data_grid(e_potential, pot_kspace);
    fft_data_grid(rho_tip, rho_kspace);
    
    // Multiply rho_kspace with pot_kspace, which equals the energy in k-space.
    // Store the values to pot_kspace to save memory.
    // Scale the values with the volume unit from the integral.
    double volume_unit = spacing.x*spacing.y*spacing.z;
    for (int ix = 0; ix < n_grid.x; ix++) {
        for (int iy = 0; iy < n_grid.y; iy++) {
            for (int iz = 0; iz < n_grid.z; iz++) {
                pot_kspace.at(ix, iy, iz) *= volume_unit*rho_kspace.at(ix, iy, iz);
            }
        }
    }
    
    // To keep up with the progress, create a correctly named reference for the pot_kspace
    DataGrid<dcomplex>& energy_kspace = pot_kspace;
    
    // rho_kspace is not needed anymore, so use it for temporary storage in k-space.
    // Temporary storage is needed in real space also, but that we have to create.
    DataGrid<dcomplex>& temp_kspace = rho_kspace;
    DataGrid<double> temp_rspace;
    temp_rspace.setOrigin(e_potential.getOrigin());
    
    // rho_tip is not needed anymore, so use the reserved memory for storing the energy
    DataGrid<double>& energy = rho_tip;
    energy.setOrigin(e_potential.getOrigin());
    
    // Do inverse FFT of rho_kspace * pot_kspace to obtain the energy
    ffti_data_grid(energy_kspace, energy);
    
    // Calculate the components of the force one by one
    vector<double> kx_points, ky_points, kz_points;
    kx_points.reserve(n_grid.x);
    ky_points.reserve(n_grid.y);
    kz_points.reserve(n_grid.z);
    for (int ix = 0; ix < n_grid.x; ix++) {
        kx_points[ix] = ix*energy_kspace.getSpacing().x;
        if (ix >= int(n_grid.x/2))
            kx_points[ix] = kx_points[ix] - n_grid.x*energy_kspace.getSpacing().x;
    }
    for (int iy = 0; iy < n_grid.y; iy++) {
        ky_points[iy] = iy*energy_kspace.getSpacing().y;
        if (iy >= int(n_grid.y/2))
            ky_points[iy] = ky_points[iy] - n_grid.y*energy_kspace.getSpacing().y;
    }
    for (int iz = 0; iz < n_grid.z; iz++) {
        kz_points[iz] = iz*energy_kspace.getSpacing().z;
        if (iz >= int(n_grid.z/2))
            kz_points[iz] = kz_points[iz] - n_grid.z*energy_kspace.getSpacing().z;
    }
    
    // Force x component
    for (int ix = 0; ix < n_grid.x; ix++) {
        for (int iy = 0; iy < n_grid.y; iy++) {
            for (int iz = 0; iz < n_grid.z; iz++) {
                temp_kspace.at(ix, iy, iz) = -2.0*PI*dcomplex(0.0, 1.0)*kx_points[ix]*energy_kspace.at(ix, iy, iz);
            }
        }
    }
    ffti_data_grid(temp_kspace, temp_rspace);
    for (int ind = 0; ind < n_grid.x*n_grid.y*n_grid.z; ind++)
        force.at(ind).x = temp_rspace.at(ind);
    
    // Force y component
    for (int ix = 0; ix < n_grid.x; ix++) {
        for (int iy = 0; iy < n_grid.y; iy++) {
            for (int iz = 0; iz < n_grid.z; iz++) {
                temp_kspace.at(ix, iy, iz) = -2.0*PI*dcomplex(0.0, 1.0)*ky_points[iy]*energy_kspace.at(ix, iy, iz);
            }
        }
    }
    ffti_data_grid(temp_kspace, temp_rspace);
    for (int ind = 0; ind < n_grid.x*n_grid.y*n_grid.z; ind++)
        force.at(ind).y = temp_rspace.at(ind);
    
    // Force z component
    for (int ix = 0; ix < n_grid.x; ix++) {
        for (int iy = 0; iy < n_grid.y; iy++) {
            for (int iz = 0; iz < n_grid.z; iz++) {
                temp_kspace.at(ix, iy, iz) = -2.0*PI*dcomplex(0.0, 1.0)*kz_points[iz]*energy_kspace.at(ix, iy, iz);
            }
        }
    }
    ffti_data_grid(temp_kspace, temp_rspace);
    for (int ind = 0; ind < n_grid.x*n_grid.y*n_grid.z; ind++)
        force.at(ind).z = temp_rspace.at(ind);
    
    // Set up force_grid_ and move the energy and force values to it 
    force_grid_.grid_points_ = n_grid;
    force_grid_.spacing_ = spacing;
    force_grid_.offset_ = origin;
    force_grid_.setPeriodic(true);
    force.swapValues(force_grid_.forces_);
    energy.swapValues(force_grid_.energies_);
}

void ElectrostaticPotentialInteraction::eval(const vector<Vec3d>& positions, vector<Vec3d>& forces, vector<double>& energies) const {
    Vec3d tip_force;
    double tip_energy;
    force_grid_.interpolate(positions[1], tip_force, tip_energy);
    forces[1] += tip_force;
    energies[1] += tip_energy;
}

void GridInteraction::eval(const vector<Vec3d>& positions, vector<Vec3d>& forces, vector<double>& energies) const {
    Vec3d tip_force;
    double tip_energy;
    force_grid_.interpolate(positions[1], tip_force, tip_energy);
    forces[1] += tip_force;
    energies[1] += tip_energy;
}

void TipHarmonicInteraction::eval(const vector<Vec3d>& positions, vector<Vec3d>& forces, vector<double>& energies) const {
    Vec3d r_vec = positions[atom_i1_] - positions[atom_i2_];
    Vec2d r_2d = r_vec.getXY();
    double r = r_2d.len();
    double dr = r - r0_;
    double e = k_ * dr * dr;
    Vec3d f;
    if (r > TOLERANCE) {
        f = Vec3d((-2 * k_ * dr / r * r_2d), 0);
    }
    energies[atom_i1_] += e;
    energies[atom_i2_] += e;
    forces[atom_i1_] += f;
    forces[atom_i2_] -= f;
}

void XYHarmonicInteraction::eval(const vector<Vec3d>& positions, vector<Vec3d>& forces, vector<double>& energies) const {
    Vec2d r_2d = positions[atom_i_].getXY() - p0_;
    double r = r_2d.len();
    double e = k_ * r * r;
    Vec3d f;
    if (r > TOLERANCE) {
        f = Vec3d((-2 * k_ * r_2d), 0);
    }
    energies[atom_i_] += e;
    forces[atom_i_] += f;
}

void SubstrateInteraction::eval(const vector<Vec3d>& positions, vector<Vec3d>& forces, vector<double>& energies) const {
    double dz = positions[atom_i_].z - z0_;
    double sig_z = sig_ / dz;
    double sig_z2 = sig_z * sig_z;
    double sig_z4 = sig_z2 * sig_z2;
    double sig_z10 = sig_z4 * sig_z4 * sig_z2;
    double eterm = 0.0;
    double fterm = 0.0;
    if (dz <= rc_) {
      eterm += multiplier_ * ( 2./5 * sig_z10 - sig_z4) + ulj_ * dz * dz - ushift_;
      fterm += 4 * multiplier_ / dz * (sig_z10 - sig_z4) + 2 * ulj_ * dz;
    }
    forces[atom_i_].z += fterm;
    energies[atom_i_] += eterm;
}

void HarmonicInteraction::eval(const vector<Vec3d>& positions, vector<Vec3d>& forces, vector<double>& energies) const {
    Vec3d r_vec = positions[atom_i1_] - positions[atom_i2_];
    double r = r_vec.len();
    double dr = r - r0_;
    double e = k_ * pow(dr, 2);
    Vec3d f = -2 * k_ * dr / r * r_vec;
    energies[atom_i1_] += e;
    energies[atom_i2_] += e;
    forces[atom_i1_] += f;
    forces[atom_i2_] -= f;
}

void HarmonicAngleInteraction::eval(const vector<Vec3d>& positions, vector<Vec3d>& forces, vector<double>& energies) const {
    Vec3d r1_vec = positions[atom_i1_] - positions[shared_i_];
    Vec3d r2_vec = positions[atom_i2_] - positions[shared_i_];
    double r1 = r1_vec.len();
    double r2 = r2_vec.len();
    double cos_t = r1_vec.dot(r2_vec) / (r1 * r2);
    if (cos_t > 1) {
        cos_t = 1;
    } else if (cos_t < -1) {
        cos_t = -1;
    }
    double theta = acos(cos_t);
    double sin_t = sin(theta);
    double d_theta = theta - theta0_;
    double f_multiplier = -2 * k_ * d_theta / sin_t;
    Vec3d f1 = -f_multiplier / r1 * (r2_vec / r2 - r1_vec * cos_t / r1);
    Vec3d f2 = -f_multiplier / r2 * (r1_vec / r1 - r2_vec * cos_t / r2);
    double e = k_ * pow(d_theta, 2);
    forces[atom_i1_] += f1;
    forces[atom_i2_] += f2;
    forces[shared_i_] -= f1 + f2;
    energies[atom_i1_] += e;
    energies[atom_i2_] += e;
    energies[shared_i_] += e;
}

void HarmonicDihedralInteraction::eval(const vector<Vec3d>& positions, vector<Vec3d>& forces, vector<double>& energies) const {
    Vec3d r12_vec = positions[atom_i1_] - positions[atom_i2_];
    Vec3d r23_vec = positions[atom_i2_] - positions[atom_i3_];
    Vec3d r43_vec = positions[atom_i4_] - positions[atom_i3_];
    double r23 = r23_vec.len();
    Vec3d m_vec = r12_vec.cross(r23_vec);
    Vec3d n_vec = r43_vec.cross(r23_vec);
    double m = m_vec.len();
    double n = n_vec.len();
    double tan_s = n_vec.dot(r12_vec) * r23 / m_vec.dot(n_vec);
    double sigma = atan(tan_s);
    double d_sigma = sigma - sigma0_;
    double f_multiplier = 2 * k_ * d_sigma;
    Vec3d f1 = -f_multiplier * r23 / (m * m) * m_vec;
    Vec3d f2 = -f_multiplier * (r43_vec.dot(r23_vec) / (n * n * r23) * n_vec
                - ((r23*r23) + r12_vec.dot(r23_vec)) / (m * m * r23) * m_vec);
    Vec3d f3 = -f_multiplier * (r12_vec.dot(r23_vec) / (m * m * r23) * m_vec
                + ((r23*r23) - r43_vec.dot(r23_vec)) / (n * n * r23) * n_vec);
    Vec3d f4 = f_multiplier * r23 / (n * n) * n_vec;
    double e = k_ * pow(d_sigma, 2);
    forces[atom_i1_] += f1;
    forces[atom_i2_] += f2;
    forces[atom_i3_] += f3;
    forces[atom_i4_] += f4;
    energies[atom_i1_] += e;
    energies[atom_i2_] += e;
    energies[atom_i3_] += e;
    energies[atom_i4_] += e;
}
