#pragma once
#include "error_handling.hpp"
#include "random.hpp"
#include <algorithm>
#include <glm/glm.hpp>
#include <memory>
#include <sparsehash/dense_hash_set>
#include <vector>
using namespace glm;

struct Packed_UG {
  // (arena_origin, arena_size)
  std::vector<uint> arena_table;
  // [point_id..]
  std::vector<uint> ids;
};

// Uniform Grid
//  ____
// |    |}size
// |____|}size
//
struct UG {
  float size;
  uint bin_count;
  std::vector<std::vector<uint>> bins;
  std::vector<uint> bins_indices;
  UG(float size, u32 bin_count) : size(size), bin_count(bin_count) {
    bins.push_back({});
    for (u32 i = 0; i < bin_count * bin_count * bin_count; i++)
      bins_indices.push_back(0);
  }
  Packed_UG pack() {
    Packed_UG out;
    out.ids.push_back(0);
    for (auto &bin_index : bins_indices) {
      if (bin_index > 0) {
        auto &bin = bins[bin_index];
        out.arena_table.push_back(out.ids.size());
        out.arena_table.push_back(bin.size());
        for (auto &id : bin) {
          out.ids.push_back(id);
        }
      } else {
        out.arena_table.push_back(0);
        out.arena_table.push_back(0);
      }
    }
    return out;
  }
  void put(vec3 const &pos, float radius, uint index) {
    if (pos.x > this->size + radius || pos.y > this->size + radius ||
        pos.z > this->size + radius || pos.x < -this->size - radius ||
        pos.y < -this->size - radius || pos.z < -this->size - radius) {
      panic("");
      return;
    }
    const auto bin_size = (2.0f * this->size) / float(this->bin_count);
    ivec3 min_ids =
        ivec3((pos + vec3(size, size, size) - vec3(radius, radius, radius)) /
              bin_size);
    ivec3 max_ids =
        ivec3((pos + vec3(size, size, size) + vec3(radius, radius, radius)) /
              bin_size);
    for (int ix = min_ids.x; ix <= max_ids.x; ix++) {
      for (int iy = min_ids.y; iy <= max_ids.y; iy++) {
        for (int iz = min_ids.z; iz <= max_ids.z; iz++) {
          // Boundary check
          if (ix < 0 || iy < 0 || iz < 0 || ix >= int(this->bin_count) ||
              iy >= int(this->bin_count) || iz >= int(this->bin_count)) {
            continue;
          }
          u32 flat_id = ix + iy * this->bin_count +
                        iz * this->bin_count * this->bin_count;
          auto *bin_id = &this->bins_indices[flat_id];
          if (*bin_id == 0) {
            this->bins.push_back({});
            *bin_id = this->bins.size() - 1;
          }
          this->bins[*bin_id].push_back(index);
        }
      }
    }
  }
  // pub fn fill_lines_render(&self, lines: &mut Vec<vec3>) {
  //     const auto bin_size = this->size * 2.0 / this->bin_count as f32;
  //     for dz in 0..this->bin_count {
  //         for dy in 0..this->bin_count {
  //             for dx in 0..this->bin_count {
  //                 const auto flat_id = dx + dy * this->bin_count + dz *
  //                 this->bin_count * this->bin_count; const auto bin_id =
  //                 &this->bins_indices[flat_id as usize]; if *bin_id != 0 {
  //                     const auto bin_idx = bin_size * dx as f32 - this->size;
  //                     const auto bin_idy = bin_size * dy as f32 - this->size;
  //                     const auto bin_idz = bin_size * dz as f32 - this->size;
  //                     const auto iter_x = [0, 0, 1, 1, 0, 0, 1, 1, 0, 0];
  //                     const auto iter_y = [0, 1, 1, 0, 0, 0, 0, 1, 1, 0];
  //                     const auto iter_z = [0, 0, 0, 0, 0, 1, 1, 1, 1, 1];
  //                     for i in 0..9 {
  //                         lines.push(vec3 {
  //                             x: bin_idx + bin_size * iter_x[i] as f32,
  //                             y: bin_idy + bin_size * iter_y[i] as f32,
  //                             z: bin_idz + bin_size * iter_z[i] as f32,
  //                         });
  //                         lines.push(vec3 {
  //                             x: bin_idx + bin_size * iter_x[i + 1] as f32,
  //                             y: bin_idy + bin_size * iter_y[i + 1] as f32,
  //                             z: bin_idz + bin_size * iter_z[i + 1] as f32,
  //                         });
  //                     }
  //                     const auto iter_x = [0, 0, 1, 1, 1, 1,];
  //                     const auto iter_y = [1, 1, 1, 1, 0, 0,];
  //                     const auto iter_z = [0, 1, 0, 1, 0, 1,];
  //                     for i in 0..3 {
  //                         lines.push(vec3 {
  //                             x: bin_idx + bin_size * iter_x[i * 2] as f32,
  //                             y: bin_idy + bin_size * iter_y[i * 2] as f32,
  //                             z: bin_idz + bin_size * iter_z[i * 2] as f32,
  //                         });
  //                         lines.push(vec3 {
  //                             x: bin_idx + bin_size * iter_x[i * 2 + 1] as
  //                             f32, y: bin_idy + bin_size * iter_y[i * 2 + 1]
  //                             as f32, z: bin_idz + bin_size * iter_z[i * 2 +
  //                             1] as f32,
  //                         });
  //                     }
  //                 }
  //             }
  //         }
  //     }
  // }
  std::vector<u32> traverse(vec3 const &pos, f32 radius) {
    if (pos.x > this->size + radius || pos.y > this->size + radius ||
        pos.z > this->size + radius || pos.x < -this->size - radius ||
        pos.y < -this->size - radius || pos.z < -this->size - radius) {
      return {};
    }
    const auto bin_size = (2.0f * this->size) / float(this->bin_count);
    ivec3 min_ids =
        ivec3((pos + vec3(size, size, size) - vec3(radius, radius, radius)) /
              bin_size);
    ivec3 max_ids =
        ivec3((pos + vec3(size, size, size) + vec3(radius, radius, radius)) /
              bin_size);
    google::dense_hash_set<u32> set;
    set.set_empty_key(UINT32_MAX);
    for (int ix = min_ids.x; ix <= max_ids.x; ix++) {
      for (int iy = min_ids.y; iy <= max_ids.y; iy++) {
        for (int iz = min_ids.z; iz <= max_ids.z; iz++) {
          // Boundary check
          if (ix < 0 || iy < 0 || iz < 0 || ix >= int(this->bin_count) ||
              iy >= int(this->bin_count) || iz >= int(this->bin_count)) {
            continue;
          }
          u32 flat_id = ix + iy * this->bin_count +
                        iz * this->bin_count * this->bin_count;
          auto bin_id = this->bins_indices[flat_id];
          if (bin_id != 0) {
            for (auto const &item : this->bins[bin_id]) {
              set.insert(item);
            }
          }
        }
      }
    }
    std::vector<u32> out;
    out.reserve(set.size());
    for (auto &i : set)
      out.push_back(i);
    return out;
  }
};

struct Pair_Hash {
  u64 operator()(std::pair<u32, u32> const &pair) {
    return std::hash<u32>()(pair.first) ^ std::hash<u32>()(pair.second);
  }
};

struct Simulation_State {
  // Static constants
  f32 rest_length;
  f32 spring_factor;
  f32 repell_factor;
  f32 planar_factor;
  f32 bulge_factor;
  f32 cell_radius;
  f32 cell_mass;
  f32 domain_radius;
  // Dynamic state
  std::vector<vec3> particles;
  google::dense_hash_set<std::pair<u32, u32>, Pair_Hash> links;
  f32 system_size;
  Random_Factory rf;
  // Methods
  void init() {
    links.set_empty_key({UINT32_MAX, UINT32_MAX});
    links.insert({0, 1});
    particles.push_back({0.0f, 0.0f, -cell_radius});
    particles.push_back({0.0f, 0.0f, cell_radius});
    system_size = cell_radius;
  }
  void step(float dt) {
    auto const M = u32(2.0f * system_size / rest_length);
    auto ug = UG(system_size, M);
    {
      u32 i = 0;
      for (auto const &pnt : particles) {
        ug.put(pnt, 0.0f, i);
        i++;
      }
    }
    f32 const bin_size = system_size * 2.0 / M;
    std::vector<f32> force_table(particles.size());
    std::vector<vec3> new_particles = particles;
    // Repell
    {
      u32 i = 0;
      for (auto const &old_pos_0 : particles) {
        auto close_points = ug.traverse(old_pos_0, rest_length);
        vec3 new_pos_0 = new_particles[i];
        float acc_force = 0.0f;
        for (u32 j : close_points) {
          if (j <= i)
            continue;
          vec3 const old_pos_1 = particles[j];
          vec3 new_pos_1 = new_particles[j];
          f32 const dist = glm::distance(old_pos_0, old_pos_1);
          if (dist < rest_length * 0.9) {
            links.insert({i, j});
          }
          f32 const force = repell_factor * cell_mass / (dist * dist + 1.0f);
          acc_force += std::abs(force);
          auto const vforce =
              (old_pos_0 - old_pos_1) / (dist + 1.0f) * force * dt;
          new_pos_0 += vforce;
          new_pos_1 -= vforce;
          new_particles[j] = new_pos_1;
          force_table[j] += std::abs(force);
        }
        new_particles[i] = new_pos_0;
        force_table[i] += acc_force;
        i++;
      }
    }
    // Attract
    for (auto const &link : links) {
      ASSERT_PANIC(link.first < link.second);
      u32 i = link.first;
      u32 j = link.second;
      vec3 const old_pos_0 = particles[i];
      vec3 const new_pos_0 = new_particles[i];
      vec3 const old_pos_1 = particles[j];
      vec3 const new_pos_1 = new_particles[j];
      f32 const dist = rest_length - glm::distance(old_pos_0, old_pos_1);
      f32 const force = spring_factor * cell_mass * dist;
      vec3 const vforce = (old_pos_0 - old_pos_1) * (force * dt);
      new_particles[i] = new_pos_0 + vforce;
      new_particles[j] = new_pos_1 - vforce;
      force_table[i] += std::abs(force);
      force_table[j] += std::abs(force);
    }
    // Division
    {
      u32 i = 0;
      for (auto const &old_pos_0 : particles) {
        if (rf.uniform(0, 10) == 0 && force_table[i] < 20.0f) {
          new_particles.push_back(old_pos_0 + rf.rand_unit_cube() * 1.0e-3f);
        }
      }
    }

    //         // Planarization
    //         struct Planar_Target {
    //           p : state::vec3, n : u32,
    //         } auto const mut spring_target = Vec::<Planar_Target>::new ();
    //         for (i, &pnt)
    //           in state.pos.iter().enumerate() {
    //             spring_target.push(Planar_Target{
    //               p : state::vec3{x : 0.0, y : 0.0, z : 0.0},
    //               n : 0,
    //             });
    //           }
    //         for
    //           &(i, j)in &state.links {
    //             if (i, j)
    //               == (j, i) { continue; }
    //             auto const pnt_1 = state.pos[i as usize].clone();
    //             auto const pnt_2 = state.pos[j as usize].clone();
    //             // auto const dr = (pnt_1 - pnt_2);
    //             // auto const drn = dr.normalize();
    //             spring_target[i as usize].p +=
    //                 pnt_2; // + state.params.rest_length * drn;
    //             spring_target[i as usize].n += 1;
    //             spring_target[j as usize].p +=
    //                 pnt_1; // - state.params.rest_length * drn;
    //             spring_target[j as usize].n += 1;
    //           }
    //         for (i, &pnt)
    //           in state.pos.iter().enumerate() {
    //             auto const st = &spring_target[i];
    //             if
    //               st.n == 0 { continue; }
    //             auto const pt = st.p / st.n as f32;
    //             auto const pnt_1 = state.pos[i as usize].clone();
    //             auto const dist =
    //                 state.params.rest_length - pnt_1.distance(pt.clone());
    //             auto const force =
    //                 state.params.spring_factor * state.params.cell_mass *
    //                 dist;
    //             auto const vforce = dt * (pnt_1 - pt) * force;
    //             force_history[i as usize] += f32::abs(force);
    //             new_pos[i as usize] += vforce;
    //           }

    //         // Force into the domain
    //         for (i, pnt)
    //           in new_pos.iter_mut().enumerate() {
    //             auto const dist = ((pnt.x * pnt.x) + (pnt.y * pnt.y)).sqrt();
    //             auto const diff = dist - state.params.can_radius;
    //             if
    //               diff > 0.0 {
    //                 auto const k = diff / dist;
    //                 pnt.x -= pnt.x * k;
    //                 pnt.y -= pnt.y * k;
    //               }
    //             // if pnt.z < 0.0 {
    //             //     pnt.z = 0.0;
    //             // }
    //             // auto const force = -pnt.z * 4.0;
    //             // force_history[i as usize] += f32::abs(force);
    //             // pnt.z += force * dt;
    //             if
    //               pnt.z > state.params.can_radius {
    //                 pnt.z = state.params.can_radius;
    //               }
    //             if
    //               pnt.z < -state.params.can_radius {
    //                 pnt.z = -state.params.can_radius;
    //               }
    //           }

    // Apply the changes
    particles = new_particles;
    system_size = 0.0f;
    for (auto const &pnt : particles) {
      system_size = std::max(
          system_size, std::max(std::abs(pnt.x),
                                std::max(std::abs(pnt.y), std::abs(pnt.z))));
      ;
    }
    system_size += rest_length;
    
  }
};