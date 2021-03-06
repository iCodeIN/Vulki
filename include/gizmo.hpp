#pragma once
#include "device.hpp"
#include "error_handling.hpp"
#include "memory.hpp"
#include "primitives.hpp"
#include "random.hpp"
#include "render_graph.hpp"
#include "shader_compiler.hpp"

#define GLM_ENABLE_EXPERIMENTAL
#include <glm/ext.hpp>
#include <glm/glm.hpp>
using namespace glm;

#include "gelf.h"
#include "imgui.h"

#include <shaders.h>

struct Gizmo_Vertex {
  vec3 in_position;
};
struct Gizmo_Instance_Data_CPU {
  mat4 transform;
  vec3 color;
};
struct Gizmo_Push_Constants {
  mat4 view;
  mat4 proj;
};
enum class Gizmo_Geometry_Type { CYLINDER, SPHERE, CONE };
struct Gizmo_Draw_Cmd {
  Gizmo_Geometry_Type type;
  Gizmo_Instance_Data_CPU data;
};

struct Gizmo_Drag_State {
  RAW_MOVABLE(Gizmo_Drag_State)
  float size = 1.0f;
  float grab_radius = 0.2f;
  vec3 pos;
  // Normalized and orthogonal
  vec3 x_axis = vec3(1.0f, 0.0f, 0.0f), y_axis = vec3(0.0f, 1.0f, 0.0f),
       z_axis = vec3(0.0f, 0.0f, 1.0f);
  bool selected;
  bool selected_axis[3];
  bool hovered_axis[3];
  int selected_axis_id = -1;
  float old_cpa, cpa;
  void push_draw(std::vector<Gizmo_Draw_Cmd> &cmd) {
    mat4 tranform = mat4(x_axis.x, x_axis.y, x_axis.z, 0.0f, y_axis.x, y_axis.y,
                         y_axis.z, 0.0f, z_axis.x, z_axis.y, z_axis.z, 0.0f,
                         pos.x, pos.y, pos.z, 1.0f);
    // std::vector<Gizmo_Instance_Data_CPU> gizmo_instances = {
    //     {size * x_axis, size * 0.2f, vec3(1.0f, 0.0f, 0.0f)},
    //     {size * vec3(0.0f, 1.0f, 0.0f), size * 0.2f, vec3(0.0f, 1.0f, 0.0f)},
    //     {size * vec3(0.0f, 0.0f, 1.0f), size * 0.2f, vec3(0.0f, 0.0f, 1.0f)},
    //     {vec3(0.0f, 0.0f, 0.0f), size * 1.0f, vec3(1.0f, 0.0f, 0.0f),
    //      vec3(0.0f, 0.0f, 0.0f)},
    //     {vec3(0.0f, 0.0f, 0.0f), size * 1.0f, vec3(0.0f, 1.0f, 0.0f),
    //      vec3(0.0f, 0.0f, M_PI_2)},
    //     {vec3(0.0f, 0.0f, 0.0f), size * 1.0f, vec3(0.0f, 0.0f, 1.0f),
    //      vec3(0.0f, -M_PI_2, 0.0f)},
    // };
    float x_k = hovered_axis[0] ? 1.0f : 0.5f;
    float y_k = hovered_axis[1] ? 1.0f : 0.5f;
    float z_k = hovered_axis[2] ? 1.0f : 0.5f;
    cmd.push_back(Gizmo_Draw_Cmd{
        .type = Gizmo_Geometry_Type::CYLINDER,
        .data = Gizmo_Instance_Data_CPU{
            .transform = tranform *
                         glm::rotate(float(M_PI_2), vec3(0.0f, 1.0f, 0.0f)) *
                         glm::scale(size * vec3(0.025f, 0.025f, 1.0f)),
            .color = vec3(x_k, 0.0f, 0.0f)}});
    cmd.push_back(Gizmo_Draw_Cmd{
        .type = Gizmo_Geometry_Type::CYLINDER,
        .data = Gizmo_Instance_Data_CPU{
            .transform = tranform *
                         glm::rotate(-float(M_PI_2), vec3(1.0f, 0.0f, 0.0f)) *
                         glm::scale(size * vec3(0.025f, 0.025f, 1.0f)),
            .color = vec3(0.0f, y_k, 0.0f)}});
    cmd.push_back(Gizmo_Draw_Cmd{
        .type = Gizmo_Geometry_Type::CYLINDER,
        .data = Gizmo_Instance_Data_CPU{
            .transform =
                tranform *
                //  glm::rotate(float(M_PI_2), vec3(0.0f, 0.0f, 0.0f)) *
                glm::scale(size * vec3(0.025f, 0.025f, 1.0f)),
            .color = vec3(0.0f, 0.0f, z_k)}});
    // Grab spheres
    cmd.push_back(Gizmo_Draw_Cmd{
        .type = Gizmo_Geometry_Type::SPHERE,
        .data = Gizmo_Instance_Data_CPU{
            .transform = tranform * glm::translate(vec3(size, 0.0f, 0.0f)) *
                         glm::scale(grab_radius * vec3(size, size, size)),
            .color = vec3(x_k, 0.0f, 0.0f)}});
    cmd.push_back(Gizmo_Draw_Cmd{
        .type = Gizmo_Geometry_Type::SPHERE,
        .data = Gizmo_Instance_Data_CPU{
            .transform = tranform * glm::translate(vec3(0.0f, size, 0.0f)) *
                         glm::scale(grab_radius * vec3(size, size, size)),
            .color = vec3(0.0f, y_k, 0.0f)}});
    cmd.push_back(Gizmo_Draw_Cmd{
        .type = Gizmo_Geometry_Type::SPHERE,
        .data = Gizmo_Instance_Data_CPU{
            .transform = tranform * glm::translate(vec3(0.0f, 0.0f, size)) *
                         glm::scale(grab_radius * vec3(size, size, size)),
            .color = vec3(0.0f, 0.0f, z_k)}});
  }
  void on_mouse_release() {
    selected = false;
    selected_axis[0] = false;
    selected_axis[1] = false;
    selected_axis[2] = false;
    old_cpa = 0.0f;
    cpa = 0.0f;
    selected_axis_id = -1;
  }
  float get_cpa(vec3 const &ray_origin, vec3 const &ray_dir) {
    vec3 axis{};
    axis[selected_axis_id] = 1.0f;
    float b = ray_dir[selected_axis_id];
    vec3 w0 = ray_origin - pos;
    float d = dot(ray_dir, w0);
    float e = dot(axis, w0);
    float t = (b * e - d) / (1.0f - b * b);
    vec3 closest_point = ray_origin + ray_dir * t;
    return closest_point[selected_axis_id];
  }
  void on_mouse_move(vec3 const &ray_origin, vec3 const &view_dir,
                     vec3 const &ray_dir) {
    vec3 dr = pos - ray_origin;
    size = std::abs(dot(dr, view_dir)) / 8;
    vec3 sphere_pos[] = {
        pos + size * vec3(1.0f, 0.0f, 0.0f),
        pos + size * vec3(0.0f, 1.0f, 0.0f),
        pos + size * vec3(0.0f, 0.0f, 1.0f),
    };
    hovered_axis[0] = false;
    hovered_axis[1] = false;
    hovered_axis[2] = false;
    for (u32 i = 0; i < 3; i++) {
      float radius = size * 0.2f;
      float radius2 = radius * radius;
      vec3 dr = sphere_pos[i] - ray_origin;
      float dr_dot_v = glm::dot(dr, ray_dir);
      float c = dot(dr, dr) - dr_dot_v * dr_dot_v;
      if (c < radius2) {
        hovered_axis[i] = true;
      }
    }
  }
  void on_mouse_drag(vec3 const &ray_origin, vec3 const &ray_dir) {
    float cpa = get_cpa(ray_origin, ray_dir);
    pos[selected_axis_id] += cpa - old_cpa;
    old_cpa = cpa;
  }
  bool on_mouse_click(vec3 const &ray_origin, vec3 const &ray_dir) {
    on_mouse_release();
    vec3 sphere_pos[] = {
        pos + size * vec3(1.0f, 0.0f, 0.0f),
        pos + size * vec3(0.0f, 1.0f, 0.0f),
        pos + size * vec3(0.0f, 0.0f, 1.0f),
    };
    float min_dist = 10000000.0f;

    for (u32 i = 0; i < 3; i++) {
      float radius = size * 0.2f;
      float radius2 = radius * radius;
      vec3 dr = sphere_pos[i] - ray_origin;
      float dr_dot_v = glm::dot(dr, ray_dir);
      float c = dot(dr, dr) - dr_dot_v * dr_dot_v;
      if (c < radius2) {
        float t = dr_dot_v - std::sqrt(radius2 - c);
        if (t < min_dist) {
          selected_axis_id = i32(i);
          min_dist = t;
        }
      }
    }
    if (selected_axis_id >= 0) {
      selected = true;
      selected_axis[0] = selected_axis_id == 0;
      selected_axis[1] = selected_axis_id == 1;
      selected_axis[2] = selected_axis_id == 2;
      old_cpa = get_cpa(ray_origin, ray_dir);
      return true;
    }
    return false;
  }
};

struct Camera {
  float phi = M_PI / 2.0f;
  float theta = M_PI / 2.0f;
  float distance = 60.0f;
  float mx = 0.0f, my = 0.0f;
  vec3 look_at = vec3(0.0f, 0.0f, 0.0f);
  float aspect = 1.0;
  float fov = M_PI / 2.0;
  float znear = 1.0f;
  float zfar = 10.0e5f;
  //
  vec3 pos;
  mat4 view;
  mat4 proj;
  vec3 look;
  vec3 right;
  vec3 up;
  void update(vec2 jitter = vec2(0.0f, 0.0f)) {
    /*-------------------*/
    /* Update the camera */
    /*-------------------*/
    pos = vec3(sinf(theta) * cosf(phi), cos(theta), sinf(theta) * sinf(phi)) *
              distance +
          look_at;
    look = normalize(look_at - pos);
    right = normalize(cross(look, vec3(0.0f, 1.0f, 0.0f)));
    up = normalize(cross(right, look));
    //    proj = glm::perspective(fov, aspect, znear, zfar);
    proj = mat4(0.0f);
    float tanHalfFovy = std::tan(fov * 0.5f);

    proj[0][0] = 1.0f / (aspect * tanHalfFovy);
    proj[1][1] = 1.0f / (tanHalfFovy);
    proj[2][2] = zfar / (znear - zfar);
    proj[2][3] = -1.0f;
    proj[3][2] = -(zfar * znear) / (zfar - znear);

    proj[2][0] += jitter.x;
    proj[2][1] += jitter.x;
    view = glm::lookAt(pos, look_at, vec3(0.0f, 1.0f, 0.0f));
  }
  mat4 viewproj() { return proj * view; }
};

struct Gizmo_Layer {
  RAW_MOVABLE(Gizmo_Layer)
  //////////////////
  // Camera state //
  //////////////////
  Camera camera;
  bool camera_moved = false;
  vec2 camera_jitter;
  uint cur_halton_id;
  bool jitter_on = false;
  const uint MAX_HALTON = 64u;
  vec3 mouse_ray = vec3(1.0f, 0.0f, 0.0f);
  float mx, my;

  ImVec2 old_mpos{};
  float old_cpa{};
  bool mouse_last_down = false;
  bool mouse_click[3] = {false, false, false};
  std::function<void(int)> on_click;
  // Viewport for this sample's rendering
  vk::Rect2D example_viewport = vk::Rect2D({0, 0}, {32, 32});

  clock_t last_time = clock();

  // Gizmos
  Gizmo_Drag_State gizmo_drag_state;

  std::vector<Gizmo_Draw_Cmd> cmds;
  std::vector<sh_gizmo_line_vert::_Binding_0> line_segments;
  // Vulkan state
  Raw_Mesh_3p16i_Wrapper icosahedron_wrapper;
  Raw_Mesh_3p16i_Wrapper cylinder_wrapper;
  Raw_Mesh_3p16i_Wrapper cone_wrapper;

  using Gizmo_Instance_Data = sh_gizmo_vert::_Binding_1;

  void push_gizmo(Gizmo_Draw_Cmd cmd) { cmds.push_back(cmd); }
  void push_cylinder(vec3 start, vec3 end, vec3 up, float radius, vec3 color) {
    vec3 dr = end - start;
    float length = glm::length(dr);
    vec3 dir = glm::normalize(dr);
    vec3 tangent = glm::normalize(glm::cross(dir, up));
    vec3 binormal = -glm::cross(dir, tangent);
    mat4 tranform = mat4(tangent.x, tangent.y, tangent.z, 0.0f, binormal.x,
                         binormal.y, binormal.z, 0.0f, dir.x, dir.y, dir.z,
                         0.0f, start.x, start.y, start.z, 1.0f);
    // gizmo_drag_state.size *
    push_gizmo(Gizmo_Draw_Cmd{
        .type = Gizmo_Geometry_Type::CYLINDER,
        .data = Gizmo_Instance_Data_CPU{
            .transform = tranform * glm::scale(vec3(radius, radius, length)),
            .color = color}});
  }
  void push_line_arrow(vec3 start, vec3 end, vec3 up, vec3 color) {
    vec3 dr = end - start;
    vec3 dir = glm::normalize(dr);
    float length = glm::length(dr);
    float dx = length * 0.1f;
    vec3 tangent = glm::normalize(glm::cross(dir, up));
    vec3 binormal = -glm::cross(dir, tangent);
    line_segments.push_back({start, color});
    line_segments.push_back({end, color});
    line_segments.push_back({end, color});
    line_segments.push_back({end - dir * dx + tangent * dx, color});
    line_segments.push_back({end, color});
    line_segments.push_back({end - dir * dx - tangent * dx, color});
  }
  void push_line_plane(vec3 pos, vec3 up, vec3 right, vec3 color) {
    static const int signs_x[5] = {-1, 1, 1, -1, -1};
    static const int signs_y[5] = {-1, -1, 1, 1, -1};
    vec3 dir = normalize(glm::cross(right, up));
    ito(4) {
      line_segments.push_back(
          {pos + f32(signs_y[i]) * up + f32(signs_x[i]) * right, color});
      line_segments.push_back(
          {pos + f32(signs_y[i + 1]) * up + f32(signs_x[i + 1]) * right,
           color});
    }
    push_line_arrow(pos, pos + dir * 10.0f, glm::normalize(up), color);
  }
  void push_cone(vec3 start, vec3 dir, float radius, vec3 color) {
    vec3 up = dir.z > 0.99f ? vec3(0.0f, 1.0f, 0.0f) : vec3(0.0f, 0.0f, 1.0f);
    vec3 tangent = glm::normalize(glm::cross(dir, up));
    vec3 binormal = -glm::cross(dir, tangent);
    mat4 tranform = mat4(tangent.x, tangent.y, tangent.z, 0.0f, binormal.x,
                         binormal.y, binormal.z, 0.0f, dir.x, dir.y, dir.z,
                         0.0f, start.x, start.y, start.z, 1.0f);
    push_gizmo(
        Gizmo_Draw_Cmd{.type = Gizmo_Geometry_Type::CONE,
                       .data = Gizmo_Instance_Data_CPU{
                           .transform = glm::translate(start) * tranform *
                                        glm::scale(vec3(radius, radius, 1.0f)),
                           .color = color}});
  }
  void push_line_sphere(vec3 pos, float radius, vec3 color) {
    static const u32 N = 16;
    static vec2 sincos_cache[N + 1] = {};
    onetime {
      ito(N) {
        sincos_cache[i] = vec2(std::cos(f32(i) / (N - 1) * M_PI * 2.0f),
                               std::sin(f32(i) / (N - 1) * M_PI * 2.0f));
      }
      sincos_cache[N] = sincos_cache[0];
    };
    ito(N) {
      vec2 sincos1 = sincos_cache[i];
      vec2 sincos2 = sincos_cache[i + 1];
      line_segments.push_back(
          {pos + radius * vec3(sincos1.x, sincos1.y, 0.0f), color});
      line_segments.push_back(
          {pos + radius * vec3(sincos2.x, sincos2.y, 0.0f), color});
    }
    ito(N) {
      vec2 sincos1 = sincos_cache[i];
      vec2 sincos2 = sincos_cache[i + 1];
      line_segments.push_back(
          {pos + radius * vec3(sincos1.x, 0.0f, sincos1.y), color});
      line_segments.push_back(
          {pos + radius * vec3(sincos2.x, 0.0f, sincos2.y), color});
    }
    ito(N) {
      vec2 sincos1 = sincos_cache[i];
      vec2 sincos2 = sincos_cache[i + 1];
      line_segments.push_back(
          {pos + radius * vec3(0.0f, sincos1.x, sincos1.y), color});
      line_segments.push_back(
          {pos + radius * vec3(0.0f, sincos2.x, sincos2.y), color});
    }
  }
  void push_line(vec3 start, vec3 end, vec3 color) {
    line_segments.push_back({start, color});
    line_segments.push_back({end, color});
  }
  void push_sphere(vec3 start, float radius, vec3 color) {
    push_gizmo(Gizmo_Draw_Cmd{
        .type = Gizmo_Geometry_Type::SPHERE,
        .data = Gizmo_Instance_Data_CPU{
            .transform = glm::translate(start) *
                         glm::scale(vec3(radius, radius, radius)),
            .color = color}});
  }
  void draw(render_graph::Graphics_Utils &gu) {
    if (!cylinder_wrapper.index_buffer) {
      cone_wrapper =
          Raw_Mesh_3p16i_Wrapper::create(gu, subdivide_cone(8, 1.0f, 1.0f));
      icosahedron_wrapper =
          Raw_Mesh_3p16i_Wrapper::create(gu, subdivide_icosahedron(2));
      cylinder_wrapper =
          Raw_Mesh_3p16i_Wrapper::create(gu, subdivide_cylinder(8, 1.0f, 1.0f));
    }
    gizmo_drag_state.push_draw(cmds);

    if (cmds.size()) {
      std::vector<Gizmo_Draw_Cmd> cylinders;
      std::vector<Gizmo_Draw_Cmd> spheres;
      std::vector<Gizmo_Draw_Cmd> cones;
      for (auto cmd : cmds) {
        if (cmd.type == Gizmo_Geometry_Type::CYLINDER) {
          cylinders.push_back(cmd);
        } else if (cmd.type == Gizmo_Geometry_Type::SPHERE) {
          spheres.push_back(cmd);
        } else if (cmd.type == Gizmo_Geometry_Type::CONE) {
          cones.push_back(cmd);
        }
      }
      u32 gizmo_instance_buffer = gu.create_buffer(render_graph::Buffer{
          .usage_bits = vk::BufferUsageFlagBits::eVertexBuffer,
          .size = u32((cmds.size()) * sizeof(Gizmo_Instance_Data))});

      u32 cylinder_instance_offset = 0;
      u32 spheres_instance_offset = 0;
      u32 cones_instance_offset = 0;
      {
        void *data = gu.map_buffer(gizmo_instance_buffer);
        Gizmo_Instance_Data *typed_data = (Gizmo_Instance_Data *)data;
        u32 total_index = 0;
        auto push_type = [&](std::vector<Gizmo_Draw_Cmd> &arr) {
          ito(arr.size()) {
            auto data = arr[i].data;
            typed_data[total_index].in_model_0 = data.transform[0];
            typed_data[total_index].in_model_1 = data.transform[1];
            typed_data[total_index].in_model_2 = data.transform[2];
            typed_data[total_index].in_model_3 = data.transform[3];
            typed_data[total_index].in_color = data.color;
            total_index++;
          }
        };
        push_type(cylinders);
        spheres_instance_offset = total_index;
        push_type(spheres);
        cones_instance_offset = total_index;
        push_type(cones);
        gu.unmap_buffer(gizmo_instance_buffer);
      }

      {
        gu.VS_set_shader("gizmo.vert.glsl");
        gu.PS_set_shader("gizmo.frag.glsl");
        gu.RS_set_depth_stencil_state(true, vk::CompareOp::eLessOrEqual, true,
                                      1.0f);
        Gizmo_Push_Constants tmp_pc{};
        tmp_pc.proj = camera.proj;
        tmp_pc.view = camera.view;
        gu.push_constants(&tmp_pc, sizeof(Gizmo_Push_Constants));

        gu.IA_set_vertex_buffers(
            {render_graph::Buffer_Info{.buf_id = gizmo_instance_buffer,
                                       .offset = 0}},
            1);
        gu.IA_set_topology(vk::PrimitiveTopology::eTriangleList);
        gu.IA_set_cull_mode(vk::CullModeFlagBits::eNone,
                            vk::FrontFace::eCounterClockwise,
                            vk::PolygonMode::eFill, 1.0f);
        // Draw cylinders
        cylinder_wrapper.draw(gu, cylinders.size());
        // Draw spheres
        icosahedron_wrapper.draw(gu, cylinders.size(), spheres_instance_offset);
        // Draw Cones
        cone_wrapper.draw(gu, cones.size(), cones_instance_offset);
      }
      gu.release_resource(gizmo_instance_buffer);
    }

    if (line_segments.size()) {
      gu.VS_set_shader("gizmo_line.vert.glsl");
      gu.PS_set_shader("gizmo.frag.glsl");
      gu.RS_set_depth_stencil_state(true, vk::CompareOp::eLessOrEqual, true,
                                    1.0f);
      Gizmo_Push_Constants tmp_pc{};
      tmp_pc.proj = camera.proj;
      tmp_pc.view = camera.view;
      gu.push_constants(&tmp_pc, sizeof(Gizmo_Push_Constants));

      u32 gizmo_lines_buffer = gu.create_buffer(
          render_graph::Buffer{
              .usage_bits = vk::BufferUsageFlagBits::eVertexBuffer,
              .size = u32((line_segments.size() * sizeof(line_segments[0])))},
          &line_segments[0]);
      gu.IA_set_topology(vk::PrimitiveTopology::eLineList);
      gu.IA_set_cull_mode(vk::CullModeFlagBits::eNone,
                          vk::FrontFace::eCounterClockwise,
                          vk::PolygonMode::eFill, 1.0f);
      gu.IA_set_vertex_buffers({render_graph::Buffer_Info{
          .buf_id = gizmo_lines_buffer, .offset = 0}});
      gu.draw(line_segments.size(), 1, 0, 0);
      gu.release_resource(gizmo_lines_buffer);
    }

    // Reset cpu command stream
    line_segments.clear();
    cmds.clear();
  }

  void on_imgui_viewport() {
    push_line(camera.look_at, camera.look_at + vec3(0.0f, 0.0f, 1.0f),
              vec3(0.0f, 0.0f, 1.0f));
    push_line(camera.look_at, camera.look_at + vec3(0.0f, 1.0f, 0.0f),
              vec3(0.0f, 1.0f, 0.0f));
    push_line(camera.look_at, camera.look_at + vec3(1.0f, 0.0f, 0.0f),
              vec3(1.0f, 0.0f, 0.0f));
    auto cur_time = clock();
    auto dt = float(double(cur_time - last_time) / CLOCKS_PER_SEC);
    last_time = cur_time;
    /*---------------------------------------*/
    /* Update the viewport for the rendering */
    /*---------------------------------------*/
    auto wpos = ImGui::GetCursorScreenPos();
    example_viewport.offset.x = wpos.x;
    example_viewport.offset.y = wpos.y;
    auto wsize = ImGui::GetWindowSize();
    example_viewport.extent.width = wsize.x;
    float height_diff = 24;
    if (wsize.y < height_diff + 2) {
      example_viewport.extent.height = 2;
    } else {
      example_viewport.extent.height = wsize.y - height_diff;
    }

    /*-------------------*/
    /* Update the camera */
    /*-------------------*/
    if (jitter_on) {
      camera_jitter =
          vec2(halton(cur_halton_id + 1, 2), halton(cur_halton_id + 1, 3));
      camera_jitter = camera_jitter * 0.5f - 0.5f;
    } else {
      camera_jitter = vec2(0.0f, 0.0f);
    }
    cur_halton_id = (cur_halton_id + 1) % MAX_HALTON;
//    camera.fov =
//        M_PI / 8.0f + 5.0f * M_PI / 8.0f * std::exp(-camera.distance * 0.01f);
    camera.aspect =
        float(example_viewport.extent.width) / example_viewport.extent.height;
    camera.update(
        (camera_jitter * 4.0f) /
        vec2(example_viewport.extent.width, example_viewport.extent.height));
    if (ImGui::GetIO().MouseReleased[0]) {
      gizmo_drag_state.on_mouse_release();
    }
    camera_moved = false;
    if (ImGui::IsWindowHovered()) {
      ///////// Low precision timer

      ///////////////////////
      auto eps = 1.0e-4f;
      auto mpos = ImGui::GetMousePos();
      auto cr = ImGui::GetWindowContentRegionMax();
      mx = 2.0f * (float(mpos.x - example_viewport.offset.x) + 0.5f) /
               example_viewport.extent.width -
           1.0f;
      my = -2.0f * (float(mpos.y - example_viewport.offset.y) - 0.5f) /
               (example_viewport.extent.height) +
           1.0f;
      mouse_ray = normalize(camera.look / std::tan(camera.fov * 0.5f) +
                            camera.right * mx * camera.aspect + camera.up * my);
      gizmo_drag_state.on_mouse_move(camera.pos, camera.look, mouse_ray);
      // Normalize camera motion so that diagonal moves are not bigger
      float camera_speed = 4.0f * camera.distance;
      if (ImGui::GetIO().KeysDown[GLFW_KEY_LEFT_SHIFT]) {
        camera_speed = 10.0f * camera.distance;
      }
      vec3 camera_diff = vec3(0.0f, 0.0f, 0.0f);
      if (ImGui::GetIO().KeysDown[GLFW_KEY_W]) {
        camera_diff += camera.look;
      }
      if (ImGui::GetIO().KeysDown[GLFW_KEY_S]) {
        camera_diff -= camera.look;
      }
      if (ImGui::GetIO().KeysDown[GLFW_KEY_A]) {
        camera_diff -= camera.right;
      }
      if (ImGui::GetIO().KeysDown[GLFW_KEY_D]) {
        camera_diff += camera.right;
      }
      // It's always of length of 0.0 or 1.0 so just check
      if (glm::dot(camera_diff, camera_diff) > 1.0e-3f) {
        camera_moved = true;
        camera.look_at += glm::normalize(camera_diff) * camera_speed * dt;
      }

      if (ImGui::GetIO().MouseDown[0]) {
        if (!mouse_last_down) {
          mouse_click[0] = true;
          if (!gizmo_drag_state.on_mouse_click(camera.pos, mouse_ray) &&
              on_click) {
            on_click(0);
          }
        } else {
          mouse_click[0] = false;
        }

        if (mpos.x != old_mpos.x || mpos.y != old_mpos.y) {
          auto dx = mpos.x - old_mpos.x;
          auto dy = mpos.y - old_mpos.y;
          if (gizmo_drag_state.selected) {
            gizmo_drag_state.on_mouse_drag(camera.pos, mouse_ray);

          } else {
            camera.phi += dx * 1.0e-2f;
            camera.theta -= dy * 1.0e-2f;
            if (camera.phi > M_PI * 2.0f) {
              camera.phi -= M_PI * 2.0f;
            } else if (camera.phi < 0.0f) {
              camera.phi += M_PI * 2.0;
            }
            if (camera.theta > M_PI - eps) {
              camera.theta = M_PI - eps;
            } else if (camera.theta < eps) {
              camera.theta = eps;
            }
            camera_moved = true;
          }
        }
      }
      old_mpos = mpos;
      auto scroll_y = ImGui::GetIO().MouseWheel;
      if (scroll_y) {
        camera_moved = true;
        camera.distance += camera.distance * 2.e-1 * scroll_y;
        camera.distance = clamp(camera.distance, eps, 1000.0f);
      }
      mouse_last_down = ImGui::GetIO().MouseDown[0];
    }
  }

  void on_imgui_begin() {
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
  }
};
