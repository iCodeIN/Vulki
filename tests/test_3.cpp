#include "../include/assets.hpp"
#include "../include/device.hpp"
#include "../include/error_handling.hpp"
#include "../include/gizmo.hpp"
#include "../include/memory.hpp"
#include "../include/model_loader.hpp"
#include "../include/particle_sim.hpp"
#include "../include/profiling.hpp"
#include "../include/shader_compiler.hpp"
#include "f32_f16.hpp"

#include "../include/random.hpp"
#include "imgui.h"

#include "examples/imgui_impl_vulkan.h"

#include "dir_monitor/include/dir_monitor/dir_monitor.hpp"
#include "gtest/gtest.h"
#include <boost/thread.hpp>
#include <chrono>
#include <cstring>
#include <filesystem>
namespace fs = std::filesystem;

#define GLM_ENABLE_EXPERIMENTAL
#include <glm/ext.hpp>
#include <glm/glm.hpp>
using namespace glm;

#include "omp.h"

struct ISPC_Packed_UG {
  uint *bins_indices;
  uint *ids;
  float _min[3], _max[3];
  uint bin_count[3];
  float bin_size;
};
extern "C" void ispc_trace(ISPC_Packed_UG *ug, void *vertices, uint *faces,
                           vec3 *ray_dir, vec3 *ray_origin,
                           Collision *out_collision, uint *ray_count);

struct JobDesc {
  uint offset, size;
};

using JobFunc = std::function<void(JobDesc)>;

struct JobPayload {
  JobFunc func;
  JobDesc desc;
};

using WorkPayload = std::vector<JobPayload>;

TEST(graphics, model_comperession) {
}

TEST(graphics, vulkan_graphics_test_3d_models) {
  // @TODO:
  // * Fix moller and woop algorithms
  //   * They have some error checking which is not relative
  //     It explodes at small triangles
  // * Visual debugging for funky precision errors
  //   * Different types of tracing
  // * Multiple objects
  // * Object movement
  // * Use canonical, system-wide approach to numerical errors
  //   * Unify all EPSILON crap

  auto device_wrapper = init_device(true);
  auto &device = device_wrapper.device;

  Simple_Monitor simple_monitor("../shaders");
  struct Scene_Node {
    vec3 position;
    Raw_Mesh_Obj model;
    Raw_Mesh_3p32i model_simplified;
    std::vector<vec3> model_flat;
    Raw_Mesh_Obj_Wrapper model_wrapper;
    UG ug = UG(1.0f, 1.0f);
    Packed_UG packed_ug;
  };
  std::vector<Scene_Node> scene_nodes;
  std::string models_path = "models/";
  std::vector<std::string> _models;
  for (const auto &entry : fs::directory_iterator(models_path))
    if (entry.path().filename().string().find(".obj") != std::string::npos) {
      _models.push_back(entry.path().relative_path().filename().string());
    }
  std::vector<const char *> model_filenames;
  for (auto const &filename : _models)
    model_filenames.push_back(filename.c_str());
  i32 current_model = 0;
  f32 test_ug_size = 0.1f;
  bool trace_ispc = true;
  u32 jobs_per_item = 1000;
  bool use_jobs = true;
  bool draw_test_model_ug = false;
  bool trace_paths = false;
  u32 max_jobs_per_iter = 50000;
  auto load_model = [&]() {
    std::string full_path = "models/" + _models[current_model];
    auto models = load_obj_raw(full_path.c_str());
    float global_upscale = 10.0f;
    // float normalization_size = 4.0f;
    for (auto &model : models) {
      Scene_Node scene_node;
      scene_node.position = vec3(0.0f, 0.0f, 0.0f);
      scene_node.model = std::move(model);
      for (auto &vertex : scene_node.model.vertices) {
        vertex.position *= global_upscale;
      }
      // swap z-y
      for (auto &vertex : scene_node.model.vertices) {
        std::swap(vertex.position.y, vertex.position.z);
        std::swap(vertex.normal.y, vertex.normal.z);
      }
      // Scale the model
      vec3 test_model_min(0.0f, 0.0f, 0.0f), test_model_max(0.0f, 0.0f, 0.0f);
      float avg_triangle_radius = 0.0f;
      for (auto face : scene_node.model.indices) {
        vec3 v0 = scene_node.model.vertices[face.v0].position;
        vec3 v1 = scene_node.model.vertices[face.v1].position;
        vec3 v2 = scene_node.model.vertices[face.v2].position;
        vec3 triangle_min, triangle_max;
        get_aabb(v0, v1, v2, triangle_min, triangle_max);
        vec3 center;
        float radius;
        get_center_radius(v0, v1, v2, center, radius);
        avg_triangle_radius += radius;
        union_aabb(triangle_min, triangle_max, test_model_min, test_model_max);
      }
      avg_triangle_radius /= scene_node.model.indices.size();
      vec3 dim = test_model_max - test_model_min;

      scene_node.model_simplified = scene_node.model.convert_to_simplified();
      scene_node.model_flat = scene_node.model.flatten();
      scene_node.model_wrapper =
          Raw_Mesh_Obj_Wrapper::create(device_wrapper, scene_node.model);
      vec3 ug_size = test_model_max - test_model_min;
      float smallest_dim = std::min(ug_size.x, std::min(ug_size.y, ug_size.z));
      float longest_dim = std::max(ug_size.x, std::max(ug_size.y, ug_size.z));
      float ug_cell_size =
          std::max((longest_dim / 64) + 0.01f,
                   std::min(avg_triangle_radius * 2.0f, longest_dim / 2));
      scene_node.ug = UG(test_model_min, test_model_max, ug_cell_size);

      {
        u32 triangle_id = 0;
        for (auto face : scene_node.model.indices) {
          vec3 v0 = scene_node.model.vertices[face.v0].position;
          vec3 v1 = scene_node.model.vertices[face.v1].position;
          vec3 v2 = scene_node.model.vertices[face.v2].position;
          vec3 triangle_min, triangle_max;
          get_aabb(v0, v1, v2, triangle_min, triangle_max);
          scene_node.ug.put((triangle_min + triangle_max) * 0.5f,
                            (triangle_max - triangle_min) * 0.5f, triangle_id);
          triangle_id++;
        }
      }
      scene_node.packed_ug = scene_node.ug.pack();
      scene_nodes.emplace_back(std::move(scene_node));
    }
  };
  load_model();
  // Some shader data structures
  struct Test_Model_Vertex {
    vec3 in_position;
    vec3 in_normal;
    vec3 in_color;
    vec2 in_texcoord;
    u32 in_mat_id;
  };
  struct Test_Model_Instance_Data {
    vec4 in_model_0;
    vec4 in_model_1;
    vec4 in_model_2;
    vec4 in_model_3;
  };
  struct Test_Model_Push_Constants {
    mat4 view;
    mat4 proj;
  };
  struct Particle_UBO {
    mat4 world;
    mat4 view;
    mat4 proj;
  };
  struct Particle_Vertex {
    vec3 position;
  };
  auto test_model_instance_buffer = device_wrapper.alloc_state->allocate_buffer(
      vk::BufferCreateInfo()
          .setSize(1 * sizeof(Test_Model_Instance_Data))
          .setUsage(vk::BufferUsageFlagBits::eVertexBuffer),
      VMA_MEMORY_USAGE_CPU_TO_GPU);
  VmaBuffer ug_lines_gpu_buffer;
  Gizmo_Layer gizmo_layer{};

  ////////////////////////
  // Path tracing state //
  ////////////////////////
  Random_Factory frand;
  struct Path_Tracing_Camera {
    vec3 pos;
    vec3 look;
    vec3 up;
    vec3 right;
    f32 fov;
    vec2 _debug_uv;
    vec3 gen_ray(f32 u, f32 v) {
      return normalize(look + up * v + right * u * fov);
    }
  } path_tracing_camera;

  struct Path_Tracing_Image {
    std::vector<vec4> data;
    u32 width, height;
    void init(u32 _width, u32 _height) {
      width = _width;
      height = _height;
      // Pitch less flat array
      data.clear();
      data.resize(width * height);
    }
    void add_value(u32 x, u32 y, vec4 val) { data[x + y * width] += val; }
    vec4 get_value(u32 x, u32 y) { return data[x + y * width]; }
  } path_tracing_image;
  struct Path_Tracing_Job {
    vec3 ray_origin, ray_dir;
    vec3 color;
    u32 pixel_x, pixel_y;
    f32 weight;
    u32 media_material_id;
    u32 depth;
  };
  struct Path_Tracing_Queue {
    std::deque<Path_Tracing_Job> job_queue;
    Path_Tracing_Job dequeue() {
      auto back = job_queue.back();
      job_queue.pop_back();
      return back;
    }
    void enqueue(Path_Tracing_Job job) { job_queue.push_front(job); }
    bool has_job() { return !job_queue.empty(); }
    void reset() { job_queue.clear(); }
  } path_tracing_queue;
  CPU_Image path_tracing_gpu_image;

  auto grab_path_tracing_cam = [&] {
    path_tracing_camera.pos = gizmo_layer.camera_pos;
    path_tracing_camera.look = gizmo_layer.camera_look;
    path_tracing_camera.up = gizmo_layer.camera_up;
    path_tracing_camera.right = gizmo_layer.camera_right;
    path_tracing_camera.fov = float(gizmo_layer.example_viewport.extent.width) /
                              gizmo_layer.example_viewport.extent.height;
  };
  auto reset_path_tracing_state = [&](u32 width, u32 height) {
    grab_path_tracing_cam();
    path_tracing_queue.reset();
    path_tracing_image.init(width, height);
    path_tracing_gpu_image = CPU_Image::create(device_wrapper, width, height,
                                               vk::Format::eR8G8B8A8Unorm);
    ito(height) {
      jto(width) {
        // 16 samples per pixel
        kto(16) {
          f32 jitter_u = halton(k + 1, 2);
          f32 jitter_v = halton(k + 1, 3);
          f32 u = (f32(j) + jitter_u) / width * 2.0f - 1.0f;
          f32 v = -(f32(i) + jitter_v) / height * 2.0f + 1.0f;
          Path_Tracing_Job job;
          job.ray_dir = path_tracing_camera.gen_ray(u, v);
          job.ray_origin = path_tracing_camera.pos;
          job.pixel_x = j;
          job.pixel_y = i;
          job.weight = 1.0f;
          job.color = vec3(1.0f, 1.0f, 1.0f);
          job.depth = 0;
          path_tracing_queue.enqueue(job);
        }
      }
    }
  };

  auto path_tracing_iteration = [&] {
    auto light_value = [](vec3 ray_dir, vec3 color) {
      float brightness = (0.5f * (0.5f + 0.5f * ray_dir.z));
      float r = (0.01f * std::pow(0.5f - 0.5f * ray_dir.z, 4.0f));
      float g = (0.01f * std::pow(0.5f - 0.5f * ray_dir.x, 4.0f));
      float b = (0.01f * std::pow(0.5f - 0.5f * ray_dir.y, 4.0f));
      return vec4(color.x * (brightness + r), color.x * (g + brightness),
                  color.x * (brightness + b), 1.0f);
    };
    if (trace_ispc) {

      u32 jobs_sofar = 0;
      std::vector<Path_Tracing_Job> ray_jobs;
      std::vector<vec3> ray_dirs;
      std::vector<vec3> ray_origins;
      std::vector<Collision> ray_collisions;
      while (path_tracing_queue.has_job()) {
        jobs_sofar++;
        if (jobs_sofar == max_jobs_per_iter)
          break;
        auto job = path_tracing_queue.dequeue();
        ray_jobs.push_back(job);
        ray_dirs.push_back(job.ray_dir);
        ray_origins.push_back(job.ray_origin);
        ray_collisions.push_back(Collision{.t = FLT_MAX});
      }
      // @PathTracing
      if (jobs_sofar > 0) {
        if (use_jobs) {
          WorkPayload work_payload;

          ito((ray_jobs.size() + jobs_per_item - 1) / jobs_per_item) {
            work_payload.push_back(JobPayload{
                .func =
                    [&](JobDesc desc) {
                      for (auto &node : scene_nodes) {
                        ISPC_Packed_UG ispc_packed_ug;
                        ispc_packed_ug.ids = &node.packed_ug.ids[0];
                        ispc_packed_ug.bins_indices =
                            &node.packed_ug.arena_table[0];
                        memcpy(ispc_packed_ug._min, &node.packed_ug.min, 12);
                        memcpy(ispc_packed_ug._max, &node.packed_ug.max, 12);
                        memcpy(ispc_packed_ug.bin_count,
                               &node.packed_ug.bin_count, 12);
                        ispc_packed_ug.bin_size = node.packed_ug.bin_size;
                        uint _tmp = desc.size;
                        ispc_trace(&ispc_packed_ug, (void *)&node.model_flat[0],
                                   (uint *)&node.model_simplified.indices[0],
                                   &ray_dirs[desc.offset],
                                   &ray_origins[desc.offset],
                                   &ray_collisions[desc.offset], &_tmp);
                      }
                    },
                .desc = JobDesc{
                    .offset = i * jobs_per_item,
                    .size = std::min(u32(ray_jobs.size()) - i * jobs_per_item,
                                     jobs_per_item)}});
          }
#pragma omp parallel for
          for (u32 i = 0; i < work_payload.size(); i++) {
            auto work = work_payload[i];
            work.func(work.desc);
          }

        } else {
          for (auto &node : scene_nodes) {
            ISPC_Packed_UG ispc_packed_ug;
            ispc_packed_ug.ids = &node.packed_ug.ids[0];
            ispc_packed_ug.bins_indices = &node.packed_ug.arena_table[0];
            memcpy(ispc_packed_ug._min, &node.packed_ug.min, 12);
            memcpy(ispc_packed_ug._max, &node.packed_ug.max, 12);
            memcpy(ispc_packed_ug.bin_count, &node.packed_ug.bin_count, 12);
            ispc_packed_ug.bin_size = node.packed_ug.bin_size;
            uint _tmp = ray_jobs.size();
            ispc_trace(&ispc_packed_ug, (void *)&node.model_flat[0],
                       (uint *)&node.model_simplified.indices[0], &ray_dirs[0],
                       &ray_origins[0], &ray_collisions[0], &_tmp);
          }
        }
        ito(ray_jobs.size()) {
          auto job = ray_jobs[i];
          auto min_col = ray_collisions[i];
          if (min_col.t < FLT_MAX) {

            // path_tracing_image.add_value(job.pixel_x, job.pixel_y,
            //                              vec4(1.0f, 1.0f, 1.0f, 1.0f));
            if (job.depth == 3) {
              // Terminate
              path_tracing_image.add_value(job.pixel_x, job.pixel_y,
                                           vec4(0.0f, 0.0f, 0.0f, job.weight));
            } else {
              vec3 tangent =
                  glm::normalize(glm::cross(job.ray_dir, min_col.normal));
              vec3 binormal = glm::cross(tangent, min_col.normal);
              u32 secondary_N = 4 / (1 << job.depth);
              ito(secondary_N) {
                vec3 rand = frand.rand_unit_sphere();
                auto new_job = job;
                new_job.ray_dir =
                    glm::normalize(min_col.normal * (1.0f + rand.z) +
                                   tangent * rand.x + binormal * rand.y);
                new_job.ray_origin = min_col.position;
                new_job.weight *= 1.0f / secondary_N * 0.5f;
                new_job.depth += 1;
                path_tracing_queue.enqueue(new_job);
              }
            }
          } else {
            path_tracing_image.add_value(
                job.pixel_x, job.pixel_y,
                job.weight * light_value(job.ray_dir, vec3(1.0f, 1.0f, 1.0f)));
          }
        }
      }
    } else {
      u32 max_jobs_per_iter = 1000;
      u32 jobs_sofar = 0;
      while (path_tracing_queue.has_job()) {
        jobs_sofar++;
        if (jobs_sofar == max_jobs_per_iter)
          break;
        auto job = path_tracing_queue.dequeue();
        bool col_found = false;
        Collision min_col{.t = 1.0e10f};
        for (auto &node : scene_nodes) {
          node.ug.iterate(job.ray_dir, job.ray_origin,
                          [&](std::vector<u32> const &items, float t_max) {
                            bool any_hit = false;
                            for (u32 face_id : items) {
                              auto face = node.model.indices[face_id];
                              vec3 v0 = node.model.vertices[face.v0].position;
                              vec3 v1 = node.model.vertices[face.v1].position;
                              vec3 v2 = node.model.vertices[face.v2].position;
                              Collision col = {};

                              if (ray_triangle_test_woop(job.ray_origin,
                                                         job.ray_dir, v0, v1,
                                                         v2, col)) {
                                if (col.t < min_col.t && col.t < t_max) {
                                  min_col = col;
                                  col.mesh_id = 0;
                                  col.face_id = face_id;
                                  col_found = any_hit = true;
                                }
                              }
                            }

                            return !any_hit;
                          });
        }
        if (col_found) {
          // path_tracing_image.add_value(job.pixel_x, job.pixel_y,
          //                              vec4(1.0f, 1.0f, 1.0f, 1.0f));
          if (job.depth == 2) {
            // Terminate
            path_tracing_image.add_value(job.pixel_x, job.pixel_y,
                                         vec4(0.0f, 0.0f, 0.0f, job.weight));
          } else {
            vec3 tangent =
                glm::normalize(glm::cross(job.ray_dir, min_col.normal));
            vec3 binormal = glm::cross(tangent, min_col.normal);
            u32 secondary_N = 16;
            // if (min_col.material_id >= 0) {
            //   auto const &mat = test_model.materials[min_col.material_id];
            //   path_tracing_image.add_value(
            //       job.pixel_x, job.pixel_y,
            //       job.weight * light_value(job.ray_dir, vec3(mat.emission[0],
            //                                                  mat.emission[1],
            //                                                  mat.emission[2])));
            // }
            ito(secondary_N) {
              vec3 rand = frand.rand_unit_sphere();
              auto new_job = job;

              new_job.ray_dir =
                  glm::normalize(min_col.normal * (1.0f + rand.z) +
                                 tangent * rand.x + binormal * rand.y);
              new_job.ray_origin = min_col.position;
              new_job.weight *= 1.0f / secondary_N;
              new_job.color = new_job.color;
              new_job.depth += 1;
              path_tracing_queue.enqueue(new_job);
            }
          }
        } else {
          path_tracing_image.add_value(job.pixel_x, job.pixel_y,
                                       job.weight *
                                           light_value(job.ray_dir, job.color));
        }
      }
    }
  };
  auto launch_debug_ray = [&](vec3 ray_origin, vec3 ray_dir) {
    path_tracing_queue.reset();
    Path_Tracing_Job job;
    job.ray_dir = ray_dir;
    job.ray_origin = ray_origin;
    job.pixel_x = 0;
    job.pixel_y = 0;
    job.weight = 1.0f;
    job.color = vec3(1.0f, 1.0f, 1.0f);
    job.depth = 0;
    path_tracing_queue.enqueue(job);
    path_tracing_image.init(1, 1);
    path_tracing_gpu_image =
        CPU_Image::create(device_wrapper, 1, 1, vk::Format::eR8G8B8A8Unorm);
    path_tracing_iteration();
  };
  struct Path_Tracing_Plane_Push {
    mat4 viewprojmodel;
  };
  ///////////////////////////
  // Particle system state //
  ///////////////////////////
  Framebuffer_Wrapper framebuffer_wrapper{};
  Pipeline_Wrapper fullscreen_pipeline;
  Pipeline_Wrapper test_model_pipeline;
  Pipeline_Wrapper lines_pipeline;
  Pipeline_Wrapper path_tracing_plane_pipeline;

  auto recreate_resources = [&] {
    framebuffer_wrapper = Framebuffer_Wrapper::create(
        device_wrapper, gizmo_layer.example_viewport.extent.width,
        gizmo_layer.example_viewport.extent.height,
        vk::Format::eR32G32B32A32Sfloat);

    fullscreen_pipeline = Pipeline_Wrapper::create_graphics(
        device_wrapper, "../shaders/tests/bufferless_triangle.vert.glsl",
        "../shaders/tests/simple_1.frag.glsl",
        vk::GraphicsPipelineCreateInfo()
            .setPInputAssemblyState(
                &vk::PipelineInputAssemblyStateCreateInfo().setTopology(
                    vk::PrimitiveTopology::eTriangleList))
            .setRenderPass(framebuffer_wrapper.render_pass.get()),
        {}, {}, {});
    path_tracing_plane_pipeline = Pipeline_Wrapper::create_graphics(
        device_wrapper, "../shaders/path_tracing_plane.vert.glsl",
        "../shaders/path_tracing_plane.frag.glsl",
        vk::GraphicsPipelineCreateInfo()
            .setPInputAssemblyState(
                &vk::PipelineInputAssemblyStateCreateInfo().setTopology(
                    vk::PrimitiveTopology::eTriangleList))
            .setRenderPass(framebuffer_wrapper.render_pass.get()),
        {}, {}, {}, sizeof(Path_Tracing_Plane_Push));
    lines_pipeline = Pipeline_Wrapper::create_graphics(
        device_wrapper, "../shaders/ug_debug.vert.glsl",
        "../shaders/ug_debug.frag.glsl",
        vk::GraphicsPipelineCreateInfo()

            .setPInputAssemblyState(
                &vk::PipelineInputAssemblyStateCreateInfo().setTopology(
                    // We want lines here
                    vk::PrimitiveTopology::eLineList))

            .setRenderPass(framebuffer_wrapper.render_pass.get()),
        {
            REG_VERTEX_ATTRIB(Particle_Vertex, position, 0,
                              vk::Format::eR32G32B32Sfloat),
        },
        {vk::VertexInputBindingDescription()
             .setBinding(0)
             .setStride(12)
             .setInputRate(vk::VertexInputRate::eVertex)},
        {}, sizeof(Test_Model_Push_Constants));
    test_model_pipeline = Pipeline_Wrapper::create_graphics(
        device_wrapper, "../shaders/3p3n2t.vert.glsl",
        "../shaders/lambert.frag.glsl",
        vk::GraphicsPipelineCreateInfo().setRenderPass(
            framebuffer_wrapper.render_pass.get()),
        {
            REG_VERTEX_ATTRIB(Test_Model_Vertex, in_position, 0,
                              vk::Format::eR32G32B32Sfloat),
            REG_VERTEX_ATTRIB(Test_Model_Vertex, in_normal, 0,
                              vk::Format::eR32G32B32Sfloat),
            REG_VERTEX_ATTRIB(Test_Model_Vertex, in_texcoord, 0,
                              vk::Format::eR32G32Sfloat),
            REG_VERTEX_ATTRIB(Test_Model_Instance_Data, in_model_0, 1,
                              vk::Format::eR32G32B32A32Sfloat),
            REG_VERTEX_ATTRIB(Test_Model_Instance_Data, in_model_1, 1,
                              vk::Format::eR32G32B32A32Sfloat),
            REG_VERTEX_ATTRIB(Test_Model_Instance_Data, in_model_2, 1,
                              vk::Format::eR32G32B32A32Sfloat),
            REG_VERTEX_ATTRIB(Test_Model_Instance_Data, in_model_3, 1,
                              vk::Format::eR32G32B32A32Sfloat),
        },
        {vk::VertexInputBindingDescription()
             .setBinding(0)
             .setStride(sizeof(Test_Model_Vertex))
             .setInputRate(vk::VertexInputRate::eVertex),
         vk::VertexInputBindingDescription()
             .setBinding(1)
             .setStride(sizeof(Test_Model_Instance_Data))
             .setInputRate(vk::VertexInputRate::eInstance)},
        {}, sizeof(Test_Model_Push_Constants));
    gizmo_layer.init_vulkan_state(device_wrapper,
                                  framebuffer_wrapper.render_pass.get());
  };
  Alloc_State *alloc_state = device_wrapper.alloc_state.get();

  // Shared sampler
  vk::UniqueSampler sampler =
      device->createSamplerUnique(vk::SamplerCreateInfo().setMaxLod(1));
  vk::UniqueSampler nearest_sampler =
      device->createSamplerUnique(vk::SamplerCreateInfo()
                                      .setMinFilter(vk::Filter::eNearest)
                                      .setMagFilter(vk::Filter::eNearest)
                                      .setMaxLod(1));
  // Init device stuff
  {

    auto &cmd = device_wrapper.graphics_cmds[0].get();
    cmd.reset(vk::CommandBufferResetFlagBits::eReleaseResources);
    cmd.begin(vk::CommandBufferBeginInfo(vk::CommandBufferUsageFlags()));
    cmd.end();
    device_wrapper.sumbit_and_flush(cmd);
  }

  /*--------------------------*/
  /* Offscreen rendering loop */
  /*--------------------------*/
  device_wrapper.pre_tick = [&](vk::CommandBuffer &cmd) {
    // Update backbuffer if the viewport size has changed
    if (simple_monitor.is_updated() ||
        framebuffer_wrapper.width !=
            gizmo_layer.example_viewport.extent.width ||
        framebuffer_wrapper.height !=
            gizmo_layer.example_viewport.extent.height) {
      recreate_resources();
    }

    /*----------------------------------*/
    /* Update the offscreen framebuffer */
    /*----------------------------------*/
    if (trace_paths) {
      path_tracing_gpu_image.transition_layout_to_general(device_wrapper, cmd);
    }
    framebuffer_wrapper.transition_layout_to_write(device_wrapper, cmd);
    framebuffer_wrapper.begin_render_pass(cmd);
    cmd.setViewport(
        0,
        {vk::Viewport(0, 0, gizmo_layer.example_viewport.extent.width,
                      gizmo_layer.example_viewport.extent.height, 0.0f, 1.0f)});

    cmd.setScissor(0, {{{0, 0},
                        {gizmo_layer.example_viewport.extent.width,
                         gizmo_layer.example_viewport.extent.height}}});
    /*----------------*/
    /* Update buffers */
    /*----------------*/
    std::vector<vec3> lines;
    for (auto &node : scene_nodes) {

      node.ug.fill_lines_render(lines);
    }
    {
      ug_lines_gpu_buffer = alloc_state->allocate_buffer(
          vk::BufferCreateInfo()
              .setSize(lines.size() * sizeof(vec3))
              .setUsage(vk::BufferUsageFlagBits::eVertexBuffer |
                        vk::BufferUsageFlagBits::eTransferDst |
                        vk::BufferUsageFlagBits::eTransferSrc),
          VMA_MEMORY_USAGE_CPU_TO_GPU);
      void *data = ug_lines_gpu_buffer.map();
      vec3 *typed_data = (vec3 *)data;
      memcpy(typed_data, &lines[0], lines.size() * sizeof(vec3));
      ug_lines_gpu_buffer.unmap();
    }
    {

      void *data = test_model_instance_buffer.map();
      Test_Model_Instance_Data *typed_data = (Test_Model_Instance_Data *)data;
      for (u32 i = 0; i < 1; i++) {
        mat4 translation = glm::translate(
            vec3(0.0f, 0.0f,
                 0.0f)); // *
                         //  glm::rotate(f32(M_PI / 2), vec3(1.0f, 0.0f, 0.0f));
        typed_data[i].in_model_0 = translation[0];
        typed_data[i].in_model_1 = translation[1];
        typed_data[i].in_model_2 = translation[2];
        typed_data[i].in_model_3 = translation[3];
      }
      test_model_instance_buffer.unmap();
    }
    if (trace_paths) {
      path_tracing_plane_pipeline.bind_pipeline(device_wrapper.device.get(),
                                                cmd);
      Path_Tracing_Plane_Push tmp_pc{};
      f32 dist = 1.0f;
      vec3 pos = path_tracing_camera.pos + path_tracing_camera.look * dist;
      vec3 up = path_tracing_camera.up * dist;
      vec3 right = path_tracing_camera.right * dist * path_tracing_camera.fov;
      vec3 look = path_tracing_camera.look;
      mat4 transform;
      transform[0] = vec4(right.x, right.y, right.z, 0.0f);
      transform[1] = vec4(up.x, up.y, up.z, 0.0f);
      transform[2] = vec4(look.x, look.y, look.z, 0.0f);
      transform[3] = vec4(pos.x, pos.y, pos.z, 1.0f);
      tmp_pc.viewprojmodel =
          gizmo_layer.camera_proj * gizmo_layer.camera_view * transform;
      path_tracing_plane_pipeline.push_constants(
          cmd, &tmp_pc, sizeof(Path_Tracing_Plane_Push));
      path_tracing_plane_pipeline.update_sampled_image_descriptor(
          device.get(), "tex", path_tracing_gpu_image.image_view.get(),
          nearest_sampler.get());
      cmd.draw(6, 1, 0, 0);
    }
    {
      test_model_pipeline.bind_pipeline(device_wrapper.device.get(), cmd);
      Test_Model_Push_Constants tmp_pc{};

      tmp_pc.proj = gizmo_layer.camera_proj;
      tmp_pc.view = gizmo_layer.camera_view;
      test_model_pipeline.push_constants(cmd, &tmp_pc,
                                         sizeof(Test_Model_Push_Constants));
      for (auto &node : scene_nodes) {
        cmd.bindVertexBuffers(0,
                              {node.model_wrapper.vertex_buffer.buffer,
                               test_model_instance_buffer.buffer},
                              {0, 0});
        cmd.bindIndexBuffer(node.model_wrapper.index_buffer.buffer, 0,
                            vk::IndexType::eUint32);

        cmd.drawIndexed(node.model_wrapper.vertex_count, 1, 0, 0, 0);
      }
    }
    if (draw_test_model_ug) {
      lines_pipeline.bind_pipeline(device_wrapper.device.get(), cmd);
      Test_Model_Push_Constants tmp_pc{};

      tmp_pc.proj = gizmo_layer.camera_proj;
      tmp_pc.view = gizmo_layer.camera_view;
      lines_pipeline.push_constants(cmd, &tmp_pc,
                                    sizeof(Test_Model_Push_Constants));
      cmd.bindVertexBuffers(0,
                            {
                                ug_lines_gpu_buffer.buffer,
                            },
                            {0});

      cmd.draw(lines.size(), 1, 0, 0);
    }
    gizmo_layer.draw(device_wrapper, cmd);
    fullscreen_pipeline.bind_pipeline(device.get(), cmd);

    framebuffer_wrapper.end_render_pass(cmd);
    framebuffer_wrapper.transition_layout_to_read(device_wrapper, cmd);
  };

  /////////////////////
  // Render the image
  /////////////////////
  device_wrapper.on_tick = [&](vk::CommandBuffer &cmd) {

  };

  /////////////////////
  // Render the GUI
  /////////////////////
  device_wrapper.on_gui = [&] {
    static bool show_demo = true;
    ImGuiWindowFlags window_flags =
        ImGuiWindowFlags_MenuBar | ImGuiWindowFlags_NoDocking;
    ImGuiViewport *viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->Pos);
    ImGui::SetNextWindowSize(viewport->Size);
    ImGui::SetNextWindowViewport(viewport->ID);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    window_flags |= ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse |
                    ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove;
    window_flags |=
        ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus;

    window_flags |= ImGuiWindowFlags_NoBackground;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::SetNextWindowBgAlpha(-1.0f);
    ImGui::Begin("DockSpace Demo", nullptr, window_flags);
    ImGui::PopStyleVar();
    ImGui::PopStyleVar(2);
    ImGuiID dockspace_id = ImGui::GetID("MyDockSpace");
    ImGui::DockSpace(dockspace_id, ImVec2(0.0f, 0.0f),
                     ImGuiDockNodeFlags_PassthruCentralNode);
    ImGui::End();

    ImGui::SetNextWindowBgAlpha(-1.0f);

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::Begin("dummy window");
    ImGui::PopStyleVar(3);
    gizmo_layer.on_imgui_viewport();

    // ImGui::Button("Press me");

    ImGui::Image(ImGui_ImplVulkan_AddTexture(
                     sampler.get(), framebuffer_wrapper.image_view.get(),
                     VkImageLayout::VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL),
                 ImVec2(gizmo_layer.example_viewport.extent.width,
                        gizmo_layer.example_viewport.extent.height),
                 ImVec2(0.0f, 1.0f), ImVec2(1.0f, 0.0f));

    ImGui::End();
    ImGui::ShowDemoWindow(&show_demo);
    ImGui::Begin("Simulation parameters");

    ImGui::End();
    ImGui::Begin("Rendering configuration");
    {
      if (ImGui::GetIO().MouseDown[2]) {
        sleep(0);
      }
      Collision min_col{.t = 1.0e10f};
      bool col_found = false;
      u32 mesh_id = 0;
      for (auto &node : scene_nodes) {
        node.ug.iterate(gizmo_layer.mouse_ray, gizmo_layer.camera_pos,
                        [&](std::vector<u32> const &items, float t_max) {
                          bool any_hit = false;
                          for (u32 face_id : items) {
                            auto face = node.model.indices[face_id];
                            vec3 v0 = node.model.vertices[face.v0].position;
                            vec3 v1 = node.model.vertices[face.v1].position;
                            vec3 v2 = node.model.vertices[face.v2].position;
                            Collision col = {};

                            if (ray_triangle_test_moller(gizmo_layer.camera_pos,
                                                         gizmo_layer.mouse_ray,
                                                         v0, v1, v2, col)) {
                              if (col.t < min_col.t && col.t < t_max) {
                                col.mesh_id = mesh_id;
                                col.face_id = face_id;
                                min_col = col;
                                col_found = true;
                                any_hit = true;
                              }
                            }
                          }

                          return !any_hit;
                        });
        mesh_id++;
      }
      if (col_found) {
        gizmo_layer.push_sphere(min_col.position, 0.5f, vec3(1.0f, 0.0f, 0.0f));
        if (trace_paths) {
          vec3 dr = min_col.position - path_tracing_camera.pos;
          vec3 ray_dir = glm::normalize(dr);
          vec3 tangent = glm::normalize(glm::cross(ray_dir, min_col.normal));
          vec3 binormal = glm::cross(min_col.normal, tangent);
          gizmo_layer.push_line(min_col.position, min_col.position + tangent,
                                min_col.normal, vec3(1.0f, 0.0f, 0.0f));
          gizmo_layer.push_line(min_col.position, min_col.position + binormal,
                                min_col.normal, vec3(0.0f, 1.0f, 0.0f));
          gizmo_layer.push_line(min_col.position,
                                min_col.position + min_col.normal, tangent,
                                vec3(0.0f, 0.0f, 1.0f));
          gizmo_layer.push_line(min_col.position, path_tracing_camera.pos,
                                min_col.normal, vec3(1.0f, 1.0f, 0.0f));
          mat4 path_tracing_camera_viewproj =
              glm::perspective(float(M_PI) / 2.0f, path_tracing_camera.fov,
                               1.0e-1f, 1.0e3f) *
              glm::lookAt(path_tracing_camera.pos,
                          path_tracing_camera.pos + path_tracing_camera.look,
                          vec3(0.0f, 0.0f, 1.0f));
          vec4 projected_pos = path_tracing_camera_viewproj *
                               vec4(min_col.position.x, min_col.position.y,
                                    min_col.position.z, 1.0f);
          projected_pos /= projected_pos.w;
          path_tracing_camera._debug_uv =
              vec2(projected_pos.x, projected_pos.y) * vec2(0.5f, -0.5f) +
              vec2(0.5f, 0.5f);
          // vec3 dr = min_col.position - gizmo_layer.camera_pos;
          // float scale = std::abs(glm::dot(dr, gizmo_layer.camera_look)) / 8;
          // mat4 tranform =
          //     mat4(tangent.x, tangent.y, tangent.z, 0.0f, binormal.x,
          //          binormal.y, binormal.z, 0.0f, min_col.normal.x,
          //          min_col.normal.y, min_col.normal.z, 0.0f,
          //          min_col.position.x, min_col.position.y,
          //          min_col.position.z, 1.0f);
          // gizmo_layer.push_gizmo(Gizmo_Draw_Cmd{
          //     .type = Gizmo_Geometry_Type::CYLINDER,
          //     .data = Gizmo_Instance_Data_CPU{
          //         .transform =
          //             tranform *
          //             glm::rotate(float(M_PI_2), vec3(0.0f, 1.0f, 0.0f)) *
          //             glm::scale(scale * vec3(0.025f, 0.025f, 1.0f)),
          //         .color = vec3(1.0f, 0.0f, 0.0f)}});
          // gizmo_layer.push_gizmo(Gizmo_Draw_Cmd{
          //     .type = Gizmo_Geometry_Type::CYLINDER,
          //     .data = Gizmo_Instance_Data_CPU{
          //         .transform =
          //             tranform *
          //             glm::rotate(-float(M_PI_2), vec3(1.0f, 0.0f, 0.0f)) *
          //             glm::scale(scale * vec3(0.025f, 0.025f, 1.0f)),
          //         .color = vec3(0.0f, 1.0f, 0.0f)}});
          // gizmo_layer.push_gizmo(Gizmo_Draw_Cmd{
          //     .type = Gizmo_Geometry_Type::CYLINDER,
          //     .data = Gizmo_Instance_Data_CPU{
          //         .transform =
          //             tranform * glm::scale(scale * vec3(0.025f,
          //             0.025f, 1.0f)),
          //         .color = vec3(0.0f, 0.0f, 1.0f)}});
        }
      }

      // @Debug
      // if (col_found) {
      //   ISPC_Packed_UG ispc_packed_ug;
      //   ispc_packed_ug.ids = &test_model_packed_ug.ids[0];
      //   ispc_packed_ug.bins_indices = &test_model_packed_ug.arena_table[0];
      //   memcpy(ispc_packed_ug._min, &test_model_packed_ug.min, 12);
      //   memcpy(ispc_packed_ug._max, &test_model_packed_ug.max, 12);
      //   memcpy(ispc_packed_ug.bin_count, &test_model_packed_ug.bin_count,
      //   12); ispc_packed_ug.bin_size = test_model_packed_ug.bin_size;
      //   Collision col;
      //   uint _tmp = 1;
      //   ispc_trace(&ispc_packed_ug, (void
      //   *)&test_model_simplified.positions[0],
      //              (uint *)&test_model_simplified.indices[0],
      //              &gizmo_layer.mouse_ray, &gizmo_layer.camera_pos, &col,
      //              &_tmp);
      //   ImGui::InputFloat3("col.normal", (float*)&col.normal, 3);
      // }
    }
    if (ImGui::ListBox("test models", &current_model, &model_filenames[0],
                       model_filenames.size()))
      load_model();
    // ImGui::InputText("test model filename", test_model_filename,
    //                  sizeof(test_model_filename));
    // if (ImGui::Button("load test model")) {
    //   load_model(test_model_filename);
    // }
    // ImGui::SliderFloat("UG cell size", &test_ug_size, 0.01f, 1.0f);
    ImGui::SliderInt("max jobs per iter", (int *)&max_jobs_per_iter, 100,
                     100000);
    ImGui::SliderInt("jobs per item", (int *)&jobs_per_item, 100, 10000);
    ImGui::Checkbox("draw test model UG", &draw_test_model_ug);
    ImGui::Checkbox("use ISPC kernel", &trace_ispc);
    ImGui::Checkbox("use multi-threading", &use_jobs);
    if (ImGui::Checkbox("trace paths", &trace_paths)) {
      if (trace_paths) {
        reset_path_tracing_state(512, 512);
      }
    }
    if (ImGui::Button("update image")) {
      if (trace_paths) {
        reset_path_tracing_state(512, 512);
      }
    }
    if (ImGui::Button("launch debug ray")) {
      launch_debug_ray(vec3(6.99091816f, 2.6676445f, 19.8999996f),
                       vec3(0.0267414041f, -0.600523651f, -0.799159765f));
    }

    if (trace_paths) {
      path_tracing_iteration();
      void *data = path_tracing_gpu_image.image.map();
      u32 *typed_data = (u32 *)data;

      auto color_to_int = [](vec4 color) {
        auto saturate = [](f32 val) {
          return std::pow(val < 0.0f ? 0.0f : (val > 1.0f ? 1.0f : val), 0.5f);
        };
        return u8(255.0f * saturate(color.x / color.w)) |
               (u8(255.0f * saturate(color.y / color.w)) << 8) |
               (u8(255.0f * saturate(color.z / color.w)) << 16) | (0xff << 24);
      };
      ito(path_tracing_image.height) {
        jto(path_tracing_image.width) {
          typed_data[i * path_tracing_image.width + j] =
              color_to_int(path_tracing_image.get_value(j, i));
        }
      }
      u32 _debug_pixel_x =
          u32(path_tracing_camera._debug_uv.x * path_tracing_image.width);
      u32 _debug_pixel_y =
          u32(path_tracing_camera._debug_uv.y * path_tracing_image.height);
      ito(3) {
        jto(3) {
          i32 pixel_x = i32(_debug_pixel_x) - 1 + j;
          i32 pixel_y = i32(_debug_pixel_y) - 1 + i;
          if (pixel_x >= 0 && pixel_x < path_tracing_image.width &&
              pixel_y >= 0 && pixel_y < path_tracing_image.height) {
            typed_data[pixel_y * path_tracing_image.width + pixel_x] =
                color_to_int(vec4(1.0f, 0.0f, 0.0f, 1.0f));
          }
        }
      }

      path_tracing_gpu_image.image.unmap();
      ImGui::Image(ImGui_ImplVulkan_AddTexture(
                       sampler.get(), path_tracing_gpu_image.image_view.get(),
                       VkImageLayout::VK_IMAGE_LAYOUT_GENERAL),
                   ImVec2(path_tracing_image.width, path_tracing_image.height),
                   ImVec2(0.0f, 0.0f), ImVec2(1.0f, 1.0f));
    }
    ImGui::End();
    ImGui::Begin("Metrics");
    // ImGui::InputFloat("mx", &mx, 0.1f, 0.1f, 2);
    // ImGui::InputFloat("my", &my, 0.1f, 0.1f, 2);
    ImGui::End();
  };
  device_wrapper.window_loop();
}


int main(int argc, char **argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}