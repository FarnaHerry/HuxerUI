#include <jni.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include <huxerui/paint.h>
#include <huxerui/render_scene.h>

#include "android_gl_effects_internal.h"
#include "android_renderer.h"
#include "graphics/effects_internal.h"
#include "graphics/geometry_internal.h"

namespace {
using namespace huxerui;
using namespace huxerui::detail;

void Check(bool condition, const char* message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}

void JavaFailure(JNIEnv* environment, const std::exception& error) {
  if (!environment->ExceptionCheck()) {
    android::LocalRef<jclass> type{environment, environment->FindClass("java/lang/AssertionError")};
    environment->ThrowNew(type.Get(), error.what());
  }
}

std::vector<std::uint8_t> Source(std::size_t width, std::size_t height) {
  std::vector<std::uint8_t> pixels(width * height * 4);
  for (std::size_t y = 0; y < height; ++y) {
    for (std::size_t x = 0; x < width; ++x) {
      const auto offset = (y * width + x) * 4;
      const auto alpha = static_cast<unsigned>(160 + ((x + y) % 96));
      pixels[offset] = static_cast<std::uint8_t>(((x * 7 + y * 3) % 256) * alpha / 255);
      pixels[offset + 1] = static_cast<std::uint8_t>(((x * 3 + y * 5) % 256) * alpha / 255);
      pixels[offset + 2] = static_cast<std::uint8_t>(((x * 2 + y * 11) % 256) * alpha / 255);
      pixels[offset + 3] = static_cast<std::uint8_t>(alpha);
    }
  }
  return pixels;
}

void ShaderFixtures() {
  AndroidBackdropGl gpu;
  Check(gpu.TryInitialize(), "Android GPU fixture requires GLES 3 and HardwareBuffer import");
  constexpr std::size_t width = 96;
  constexpr std::size_t height = 64;
  const auto source = Source(width, height);
  for (int fixture = 0; fixture < 14; ++fixture) {
    DrawBackdropCommand command{
        .shapes = {{{12, 9, 25, 28}, CornerRadii{3, 7, 11, 4}}, {{38, 12, 17, 18}, CornerRadii{5}}},
        .effect = {.blur_radius = fixture == 0 ? 0.0F : 6.0F,
                   .refraction = fixture < 4 ? 0.0F : 3.0F,
                   .edge_width = 8.0F,
                   .highlight = fixture < 4 ? 0.0F : 0.25F,
                   .saturation = fixture < 4 ? 1.0F : 1.1F,
                   .brightness = 0.9F,
                   .fusion_radius = fixture < 8 ? 0.0F : 5.0F}};
    if (fixture >= 12) {
      command.effect = BackdropEffect{.blur_radius = fixture == 12 ? 18.0F : 36.0F};
    }
    Transform2D transform;
    if (fixture % 4 == 1) {
      transform = {1.5F, 0, 0, 1.5F, 2, 1};
    }
    if (fixture % 4 == 2) {
      transform = {1.2F, 0, 0, 0.8F, 3, 5};
    }
    if (fixture % 4 == 3) {
      transform = {0, 1, -1, 0, 60, 0};
    }
    auto expected = ApplyBackdropEffect(source, width, height, width * 4, *InverseTransform(transform), command).bgra;
    auto actual = source;
    BackdropPlaneBatch plane{.commands = {{&command, transform}}, .sampling_bounds = {}, .output_bounds = {}};
    Check(gpu.Apply(plane, actual.data(), width, height, 0, 0), "Android shader did not execute");
    std::uint64_t total = 0;
    unsigned maximum = 0;
    for (std::size_t i = 0; i < actual.size(); ++i) {
      const auto difference = static_cast<unsigned>(std::abs(static_cast<int>(actual[i]) - expected[i]));
      total += difference;
      maximum = std::max(maximum, difference);
    }
    if (maximum > 3 || static_cast<double>(total) / actual.size() > 0.5) {
      throw std::runtime_error("HuxerUI Android shader fixture " + std::to_string(fixture)
          + " exceeds CPU tolerance: max=" + std::to_string(maximum)
          + " mean=" + std::to_string(static_cast<double>(total) / actual.size()));
    }
  }
  for (bool uniform : {false, true}) {
    auto pixels = source;
    if (uniform) {
      for (std::size_t i = 0; i < pixels.size(); i += 4) {
        pixels[i] = 20;
        pixels[i + 1] = 30;
        pixels[i + 2] = 40;
        pixels[i + 3] = 128;
      }
    }
    const auto expected = pixels;
    DrawBackdropCommand command{.shapes = {{{8, 8, 70, 40}, CornerRadii{12}}},
                                .effect = {.blur_radius = uniform ? 12.0F : 0.0F, .refraction = uniform ? 4.0F : 0.0F}};
    BackdropPlaneBatch batch{.commands = {{&command, {}}}, .sampling_bounds = {}, .output_bounds = {}};
    Check(gpu.Apply(batch, pixels.data(), width, height, 0, 0), "Android identity fixture did not execute");
    Check(pixels == expected, "Android identity and uniform fixtures must remain byte-exact");
  }
  const auto large = Source(1024, 512);
  DrawBackdropCommand scan{.shapes = {{{20, 20, 950, 470}, CornerRadii{24}}},
                           .effect = {.blur_radius = 36, .refraction = 4, .edge_width = 16, .highlight = 0.15F}};
  auto expected = ApplyBackdropEffect(large, 1024, 512, 4096, {}, scan).bgra;
  auto actual = large;
  const auto previous_stats = gpu.FrameStats();
  BackdropPlaneBatch batch{.commands = {{&scan, {}}}, .sampling_bounds = {}, .output_bounds = {}};
  Check(gpu.Apply(batch, actual.data(), 1024, 512, 0, 0), "Android scanline shader fixture did not execute");
  Check(gpu.FrameStats().blur_passes - previous_stats.blur_passes == 6,
        "Android large-kernel fixture must execute all blur passes");
  const auto compute_passes = gpu.FrameStats().compute_blur_passes - previous_stats.compute_blur_passes;
  Check(compute_passes == 0 || compute_passes == 6,
        "Android large-kernel fixture must use a supported shader path");
  std::uint64_t total = 0;
  unsigned maximum = 0;
  for (std::size_t i = 0; i < actual.size(); ++i) {
    const auto error = static_cast<unsigned>(std::abs(static_cast<int>(actual[i]) - expected[i]));
    maximum = std::max(maximum, error);
    total += error;
  }
  Check(maximum <= 3 && static_cast<double>(total) / actual.size() <= 0.5,
        "Android scanline shader exceeds CPU pixel tolerance");
  for (bool vertical : {false, true}) {
    const std::size_t tile_width = vertical ? 73 : 1031;
    const std::size_t tile_height = vertical ? 1031 : 73;
    const auto input = Source(tile_width, tile_height);
    for (float radius : {36.0F, 180.0F, 420.0F}) {
      DrawBackdropCommand edge{
          .shapes = {{{0, 0, static_cast<float>(tile_width), static_cast<float>(tile_height)}, CornerRadii{6}}},
          .effect = {.blur_radius = radius, .refraction = 4, .edge_width = 16}};
      const auto reference = ApplyBackdropEffect(input, tile_width, tile_height, tile_width * 4, {}, edge).bgra;
      auto result = input;
      const BackdropPlaneBatch tiles{.commands = {{&edge, {}}}, .sampling_bounds = {}, .output_bounds = {}};
      Check(gpu.Apply(tiles, result.data(), tile_width, tile_height, 0, 0),
            "Android partial scanline tiles did not execute");
      unsigned tile_maximum = 0;
      std::uint64_t tile_total = 0;
      for (std::size_t i = 0; i < result.size(); ++i) {
        const auto error = static_cast<unsigned>(std::abs(static_cast<int>(result[i]) - reference[i]));
        tile_maximum = std::max(tile_maximum, error);
        tile_total += error;
      }
      Check(tile_maximum <= 3 && static_cast<double>(tile_total) / result.size() <= 0.5,
            "Android partial scanline tiles exceed CPU pixel tolerance");
    }
  }
}

void ShapeArrayFixtures() {
  AndroidBackdropGl gpu;
  Check(gpu.TryInitialize(), "Android shape-array fixtures require GLES processing");
  constexpr std::size_t width = 256, height = 192;
  const auto source = Source(width, height);
  for (const auto quality : {BackdropBlurQuality::Precise, BackdropBlurQuality::Fast}) {
    for (const int count : {1, 2, 3, 4, 16, 17, 32, 33, 64, 17, 3, 1}) {
      DrawBackdropCommand command{.shapes = {},
                                  .effect = {.blur_radius = 0, .brightness = 0.65F, .blur_quality = quality}};
      for (int index = 0; index < count; ++index) {
        command.shapes.push_back({{8.25F + index % 8 * 30, 8.5F + index / 8 * 22,
                                   index % 11 == 5 ? 0.0F : 26.0F, 18}, CornerRadii{2, 4, 3, 1}});
      }
      for (const float fusion : {0.0F, 6.0F}) {
        command.effect.fusion_radius = fusion;
        const auto expected =
            ApplyBackdropEffect(source, width, height, width * 4, {}, command).bgra;
        auto actual = source;
        const BackdropPlaneBatch batch{.commands = {{&command, {}}},
                                       .sampling_bounds = {}, .output_bounds = {}};
        Check(gpu.Apply(batch, actual.data(), width, height, 0, 0), "Android shape-array fixture did not execute");
        unsigned maximum = 0;
        std::uint64_t total = 0;
        for (std::size_t index = 0; index < actual.size(); ++index) {
          const auto error = static_cast<unsigned>(std::abs(int(actual[index]) - expected[index]));
          maximum = std::max(maximum, error);
          total += error;
        }
        if (maximum > 3 || static_cast<double>(total) / actual.size() > 0.5) {
          throw std::runtime_error("HuxerUI Android shape-array fixture exceeds CPU tolerance: shapes="
              + std::to_string(count)
              + " fusion=" + std::to_string(fusion) + " max=" + std::to_string(maximum));
        }
      }
    }
  }
}

void RefractiveNormalFixtures() {
  AndroidBackdropGl gpu;
  Check(gpu.TryInitialize(), "Android refractive normal fixtures require GLES processing");
  constexpr std::size_t width = 256, height = 192;
  std::vector<std::uint8_t> source(width * height * 4);
  for (std::size_t y = 0; y < height; ++y) {
    for (std::size_t x = 0; x < width; ++x) {
      const auto at = (y * width + x) * 4;
      const auto alpha = static_cast<unsigned>(96 + (x * 3 + y * 5) % 128);
      source[at] = static_cast<std::uint8_t>(((x / 4 + y / 5) % 2 == 0 ? 40 : 180) * alpha / 255);
      source[at + 1] = static_cast<std::uint8_t>(((x * 9 + y * 3) % 256) * alpha / 255);
      source[at + 2] = static_cast<std::uint8_t>(((x * 7 + y * 11) % 256) * alpha / 255);
      source[at + 3] = static_cast<std::uint8_t>(alpha);
    }
  }
  for (const Transform2D mapping : {Transform2D{}, Transform2D{1, 0, 0, 1, 3, 5},
                                   Transform2D{1.2F, 0.35F, 0.2F, 0.8F}}) {
    for (const int count : {32, 64}) {
      DrawBackdropCommand command{.shapes = {},
                                  .effect = {.blur_radius = 0, .refraction = 3, .highlight = 0.2F,
                                             .brightness = 0.65F}};
      for (int index = 0; index < count; ++index) {
        command.shapes.push_back({{8.25F + index % 8 * 30, 8.5F + index / 8 * 22,
                                   index % 11 == 5 ? 0.0F : 26.0F, 18}, CornerRadii{2, 4, 3, 1}});
      }
      for (const float fusion : {0.0F, 6.0F}) {
        command.effect.fusion_radius = fusion;
        const auto expected = ApplyBackdropEffect(source, width, height, width * 4,
                                                  *InverseTransform(mapping), command).bgra;
        auto actual = source;
        const BackdropPlaneBatch batch{.commands = {{&command, mapping}},
                                       .sampling_bounds = {}, .output_bounds = {}};
        Check(gpu.Apply(batch, actual.data(), width, height, 0, 0),
              "Android refractive normal fixture did not execute");
        unsigned maximum = 0;
        std::uint64_t total = 0;
        for (std::size_t index = 0; index < actual.size(); ++index) {
          const auto error = static_cast<unsigned>(std::abs(int(actual[index]) - expected[index]));
          maximum = std::max(maximum, error);
          total += error;
        }
        if (maximum > 3 || static_cast<double>(total) / actual.size() > 0.5) {
          throw std::runtime_error("HuxerUI Android refractive normal fixture exceeds CPU tolerance: shapes="
              + std::to_string(count) + " fusion=" + std::to_string(fusion)
              + " mapping=" + std::to_string(mapping.m11) + " offset=" + std::to_string(mapping.translate_x)
              + " max=" + std::to_string(maximum));
        }
      }
    }
  }
}

void FastShaderFixtures() {
  AndroidBackdropGl gpu;
  Check(gpu.TryInitialize(), "Android fast fixtures require GLES processing");
  constexpr std::size_t width = 384, height = 288, world_width = 400;
  const auto world = Source(world_width, 304);
  DrawBackdropCommand command{.shapes = {{{120, 96, 128, 96}, CornerRadii{12}}},
                              .effect = {.blur_quality = BackdropBlurQuality::Fast}};
  const BackdropPlaneBatch batch{.commands = {{&command, {}}}, .sampling_bounds = {}, .output_bounds = {}};
  for (float radius : {0.0F, 3.0F, 18.0F, 48.0F}) {
    command.effect.blur_radius = radius;
    std::vector<std::uint8_t> reference;
    for (std::size_t origin = 0; origin < 8; ++origin) {
      std::vector<std::uint8_t> input(width * height * 4);
      for (std::size_t y = 0; y < height; ++y) {
        std::copy_n(world.begin() + ((y + origin) * world_width + origin) * 4, width * 4,
                    input.begin() + y * width * 4);
      }
      auto actual = input;
      Check(gpu.Apply(batch, actual.data(), width, height, origin, origin), "Android fast fixture did not execute");
      if (origin == 0) { reference = actual; }
      for (std::size_t y = 0; y < height; ++y) {
        for (std::size_t x = 0; x < width; ++x) {
          const auto offset = (y * width + x) * 4;
          for (std::size_t channel = 0; channel < 3; ++channel) {
            Check(actual[offset + channel] <= actual[offset + 3],
                  "Android fast blur must preserve premultiplied alpha");
          }
          const bool inside = command.shapes.front().rect.Contains(
              {static_cast<float>(x + origin) + 0.5F, static_cast<float>(y + origin) + 0.5F});
          if ((!inside || radius == 0) &&
              !std::equal(input.begin() + offset, input.begin() + offset + 4, actual.begin() + offset)) {
            throw std::runtime_error("Android fast identity or exterior pixels changed at radius="
                + std::to_string(radius) + " origin=" + std::to_string(origin)
                + " x=" + std::to_string(x) + " y=" + std::to_string(y));
          }
          if (x + origin >= 124 && x + origin < 240 && y + origin >= 100 && y + origin < 184) {
            for (std::size_t channel = 0; channel < 4; ++channel) {
              const int error = std::abs(int(actual[offset + channel])
                  - int(reference[((y + origin) * width + x + origin) * 4 + channel]));
              Check(error <= 1, "Android fast blur crop movement must preserve its sampling phase");
            }
          }
        }
      }
    }
  }
  command.shapes.front().rect = {0, 0, width, height};
  for (unsigned value : {0U, 1U, 20U, 127U, 128U, 254U, 255U}) {
    auto input = world;
    input.resize(width * height * 4);
    for (std::size_t offset = 0; offset < input.size(); offset += 4) {
      input[offset] = value / 4;
      input[offset + 1] = value / 2;
      input[offset + 2] = value;
      input[offset + 3] = value;
    }
    auto actual = input;
    Check(gpu.Apply(batch, actual.data(), width, height, 0, 0), "Android uniform fast fixture did not execute");
    for (std::size_t index = 0; index < actual.size(); ++index) {
      Check(std::abs(int(actual[index]) - int(input[index])) <= 1,
            "Android fast uniform colors must remain within one quantization unit");
    }
  }
}

void UniformByteFixtures() {
  AndroidBackdropGl gpu;
  Check(gpu.TryInitialize(), "Android byte-rounding fixtures require GPU processing");
  constexpr std::size_t width = 1031;
  for (float radius : {36.0F, 381.0F, 384.0F}) {
    DrawBackdropCommand command{.shapes = {{{0, -1, static_cast<float>(width), 3}, CornerRadii{}}},
                                .effect = {.blur_radius = radius}};
    const BackdropPlaneBatch batch{.commands = {{&command, {}}}, .sampling_bounds = {}, .output_bounds = {}};
    for (unsigned value = 0; value <= 255; ++value) {
      const std::vector<std::uint8_t> expected(width * 4, static_cast<std::uint8_t>(value));
      auto actual = expected;
      Check(gpu.Apply(batch, actual.data(), width, 1, 0, 0), "Android byte-rounding fixture did not execute");
      Check(actual == expected, "Android uniform byte values must survive blur exactly");
    }
  }
}

struct FixtureRenderer {
  AndroidRenderer renderer;
  RenderNode root;
  std::array<RenderNode, 8> nodes;
  std::uint64_t revision = 0;
};

void Scene(RenderNode& root, std::array<RenderNode, 8>& nodes, int scenario, int sample, bool source_changes) {
  const bool identity = scenario < 0;
  if (identity) {
    scenario = -scenario - 1;
  }
  const bool mask_reference = scenario >= 100 && scenario < 200;
  if (mask_reference) {
    scenario -= 100;
  }
  const bool fast = scenario >= 200 && scenario < 400;
  const bool mixed_quality = scenario >= 300 && scenario < 400;
  if (fast) {
    scenario -= mixed_quality ? 300 : 200;
  }
  const int source_sample = source_changes ? sample : 0;
  PaintContext background(root.content, {0, 0, 400, 300});
  background.DrawRect({0, 0, 400, 300}, Color::Rgb(29 + source_sample % 20, 70, 100));
  for (int i = 0; i < 24; ++i) {
    background.DrawRect(
        {static_cast<float>((i * 37 + source_sample) % 400), static_cast<float>((i * 23) % 300), 12, 40},
        Color::Rgb(120 + i * 4, 50 + i * 3, 180));
  }
  background.Finish();
  for (int i = 0; i < 6; ++i) {
    auto& node = nodes[i];
    node.offset = {25.0F + (i % 3) * 125.0F, 35.0F + (i / 3) * 140.0F};
    BackdropEffect effect{.blur_radius = 8,
                          .refraction = 3,
                          .edge_width = 8,
                          .highlight = 0.15F + (source_changes ? 0 : sample % 12) * 0.01F};
    if (identity || scenario == 11) {
      effect = BackdropEffect{.blur_radius = 0};
    } else if (scenario == 12 || (scenario >= 23 && scenario <= 25)) {
      effect = BackdropEffect{.blur_radius = 18};
    }
    if (!identity && (scenario == 26 || scenario == 29 || scenario == 31)) {
      effect = BackdropEffect{.blur_radius = 0, .refraction = 3, .edge_width = 8, .highlight = 0.2F,
                              .saturation = 0.7F, .brightness = 0.9F};
    }
    if (!identity && (scenario == 27 || scenario == 30 || scenario == 31)) effect.fusion_radius = 12;
    if (!identity && scenario == 30) {
      effect.refraction = 0;
      effect.highlight = 0;
    }
    if (fast && (!mixed_quality || i % 2 == 0)) {
      effect.blur_quality = BackdropBlurQuality::Fast;
    }
    PaintContext paint(node.content, {0, 0, 80, 60});
    if (scenario == 27 || scenario == 28 || scenario == 30 || scenario == 31) {
      paint.DrawBackdrop(std::vector<BackdropShape>{{{0, 0, 35, 60}, CornerRadii{8}},
                                                  {{40, 0, 40, 60}, CornerRadii{12}}}, effect);
    } else if (scenario != 10) {
      paint.DrawBackdrop({0, 0, 80, 60}, effect, CornerRadii{14});
    }
    paint.DrawRect({0, 0, 80, 60}, Color::Rgb(255, 255, 255, 0.12F), CornerRadii{14});
    paint.DrawText({10, 14, 65, 28}, "Glass", TextStyle{.font = Font::System(15), .foreground = Color::White()});
    paint.Finish();
    root.children.push_back(&node);
  }
  if (scenario == 23) {
    nodes[0].transform = {0, 0.5F, -1.5F, 0, 110, 0};
    root.child_clips.push_back(PushClipCommand{.rect = {10, 10, 350, 230}, .corner_radius = 12});
  } else if (scenario == 24) {
    root.children.clear();
    PaintContext repeated(root.foreground, {0, 0, 400, 300});
    for (int index = 0; index < 40; ++index) {
      repeated.DrawBackdrop({0, 0, 400, 300}, BackdropEffect{
          .blur_radius = identity ? 0.0F : 18.0F,
          .blur_quality = fast && (!mixed_quality || index % 2 == 0)
                              ? BackdropBlurQuality::Fast : BackdropBlurQuality::Precise});
    }
    repeated.DrawRect({100, 100, 20, 20}, Color::White());
    repeated.Finish();
  } else if (scenario == 25 || scenario == 29) {
    nodes[0].transform = {1.2F, 0.4F, 0.1F, 0.8F, 0, 0};
  } else if (scenario == 1) {
    nodes[1].offset = {65, 45};
  } else if (scenario == 2) {
    root.child_clips.push_back(PushClipCommand{.rect = {10, 10, 350, 230}, .corner_radius = 30});
    nodes[0].opacity = 0.6F;
    nodes[6].opacity = 0.4F;
    nodes[6].children = {&nodes[0]};
    root.children[0] = &nodes[6];
  } else if (scenario == 3) {
    Path path;
    path.MoveTo({15, 15});
    path.LineTo({330, 25});
    path.LineTo({250, 250});
    path.Close();
    root.child_clips.push_back(PushPathClipCommand{.path = path});
    nodes[0].transform = {0.9F, 0.2F, -0.2F, 0.9F, 0, 0};
  } else if (scenario == 4) {
    nodes[0].offset = {-10, -10};
    nodes[5].offset = {500, 500};
  } else if (scenario == 5) {
    nodes[6].opacity = 0.7F;
    PaintContext paint(nodes[6].content, {0, 0, 180, 90});
    paint.DrawRect({0, 0, 180, 90}, Color::Rgb(190, 20, 90));
    paint.Finish();
    root.children.insert(root.children.begin(), &nodes[6]);
  } else if (scenario == 7) {
    root.child_clips.push_back(PushClipCommand{.rect = {15, 15, 280, 180}, .corner_radius = 25});
    nodes[0].transform = {1.2F, 0.4F, 0.1F, 0.8F, 0, 0};
  } else if (scenario == 8) {
    nodes[0].offset = {-22.0F + sample * 7, -12.0F + sample * 4};
    nodes[1].offset = {25, 20};
    nodes[2].offset = {320, 270};
    nodes[3].transform = {0.98F, 0.14F, -0.14F, 0.98F, 0, 0};
  } else if (scenario == 9) {
    nodes[1].offset = {65, 45};
    PaintContext ink(root.foreground, {0, 0, 400, 300});
    ink.DrawTextRun({350, 280, 1, 1}, {130, 260}, "Positioned ink", TextStyle{Font::System(18), Color::White()});
    ink.Finish();
  } else if (scenario == 21) {
    root.children = {&nodes[0]};
    nodes[0].offset = {25.0F + sample * 2, 35.0F + sample};
  } else if ((scenario >= 13 && scenario <= 20) || scenario == 22) {
    root.child_clips.push_back(PushClipCommand{
        .rect = {scenario == 14 ? 45.125F : 45.0F, 45, 260, 180},
        .corner_radius = scenario == 16 ? 12.0F : 0.0F});
    if (scenario == 15 || scenario == 17 || scenario == 18 || scenario == 20 || scenario == 22) {
      nodes[6].child_clips.push_back(PushClipCommand{
          .rect = {scenario == 18 ? 350.0F : 60.0F, 0, 150, 280}});
      nodes[6].children = root.children;
      if (scenario == 17) {
        nodes[6].transform = {0.98F, 0.14F, -0.14F, 0.98F, 0, 0};
      } else if (scenario == 20) {
        nodes[6].transform = {-1, 0, 0, 1, 400, 0};
      } else if (scenario == 22) {
        nodes[6].transform = {0.98F, 0.14F, -0.14F, 0.98F, static_cast<float>(sample), 0};
      }
      root.children = {&nodes[6]};
    }
    if (scenario == 19) {
      nodes[0].opacity = 0.6F;
    }
  } else if (scenario == 6) {
    nodes[0].content = {};
    PaintContext first(nodes[0].content, {0, 0, 80, 60});
    first.DrawBackdrop({0, 0, 80, 60}, BackdropEffect{
        .blur_radius = identity ? 0.0F : 2.0F,
        .blur_quality = fast ? BackdropBlurQuality::Fast : BackdropBlurQuality::Precise}, CornerRadii{8});
    first.Finish();
    nodes[2].content = {};
    PaintContext third(nodes[2].content, {0, 0, 80, 60});
    third.DrawBackdrop({0, 0, 80, 60}, BackdropEffect{
        .blur_radius = identity ? 0.0F : 12.0F,
        .blur_quality = fast ? BackdropBlurQuality::Fast : BackdropBlurQuality::Precise}, CornerRadii{8});
    third.Finish();
  }
  if (mask_reference) {
    const auto convert = [](RenderNode& node) {
      for (auto& value : node.child_clips) {
        const auto* clip = std::get_if<PushClipCommand>(&value);
        if (clip != nullptr && clip->corner_radius == 0) {
          const Rect rect = clip->rect;
          Path path;
          path.MoveTo({rect.x, rect.y});
          path.LineTo({rect.x + rect.width, rect.y});
          path.LineTo({rect.x + rect.width, rect.y + rect.height});
          path.LineTo({rect.x, rect.y + rect.height});
          path.Close();
          value = PushPathClipCommand{.path = std::move(path)};
        }
      }
    };
    convert(root);
    for (auto& node : nodes) {
      convert(node);
    }
  }
}
} // namespace

extern "C" JNIEXPORT void JNICALL Java_org_huxerui_HuxerUIBackdropTest_verifyShaders(JNIEnv* environment, jclass) {
  const char* stage = "precise";
  try {
    ShaderFixtures();
    stage = "shape-array";
    ShapeArrayFixtures();
    stage = "refractive-normal";
    RefractiveNormalFixtures();
    stage = "fast";
    FastShaderFixtures();
    stage = "uniform-byte";
    UniformByteFixtures();
  } catch (const std::exception& error) {
    JavaFailure(environment, std::runtime_error("HuxerUI Android " + std::string(stage) + " fixtures: " + error.what()));
  }
}

extern "C" JNIEXPORT jlong JNICALL Java_org_huxerui_HuxerUIBackdropTest_createRenderer(JNIEnv* environment, jclass,
                                                                                       jobject host) {
  try {
    auto renderer = std::make_unique<FixtureRenderer>();
    android::LocalRef<jclass> type{environment, environment->GetObjectClass(host)};
    renderer->renderer.Initialize(environment, type.Get());
    if (environment->ExceptionCheck()) {
      return 0;
    }
    return reinterpret_cast<jlong>(renderer.release());
  } catch (const std::exception& error) {
    JavaFailure(environment, error);
    return 0;
  }
}

extern "C" JNIEXPORT void JNICALL Java_org_huxerui_HuxerUIBackdropTest_destroyRenderer(JNIEnv*, jclass, jlong handle) {
  delete reinterpret_cast<FixtureRenderer*>(handle);
}

extern "C" JNIEXPORT jlongArray JNICALL Java_org_huxerui_HuxerUIBackdropTest_drawScenario(JNIEnv* environment, jclass,
                                                                                          jlong handle, jobject host,
                                                                                          jobject canvas, jint scenario,
                                                                                          jint sample,
                                                                                          jboolean source_changes) {
  try {
    auto* fixture = reinterpret_cast<FixtureRenderer*>(handle);
    Check(fixture != nullptr, "Android backdrop test renderer is missing");
    auto* renderer = &fixture->renderer;
    Check(renderer != nullptr, "Android backdrop test renderer is missing");
    if (scenario != 99) {
      fixture->root = {};
      fixture->nodes = {};
      Scene(fixture->root, fixture->nodes, scenario, sample, source_changes == JNI_TRUE);
      ++fixture->revision;
    }
    auto& root = fixture->root;
    renderer->BeginDraw();
    renderer->DrawSlice(environment, host, canvas,
                        RenderFrame{.scene = {&root}, .damage = {}, .revision = fixture->revision}, 0,
                        std::numeric_limits<std::size_t>::max());
    if (environment->ExceptionCheck()) {
      return nullptr;
    }
    const auto stats = renderer->BackdropStats();
    const jlong values[] = {static_cast<jlong>(stats.plane_count),
                            static_cast<jlong>(stats.command_count),
                            static_cast<jlong>(stats.gpu.source_captures),
                            static_cast<jlong>(stats.gpu.blur_passes),
                            static_cast<jlong>(stats.gpu.blur_cache_hits),
                            static_cast<jlong>(stats.gpu.raster_upload_bytes),
                            static_cast<jlong>(stats.gpu.readback_bytes),
                            static_cast<jlong>(stats.gpu.raster_imports),
                            static_cast<jlong>(stats.gpu.mask_upload_bytes),
                            static_cast<jlong>(stats.gpu.blur_pixels),
                            static_cast<jlong>(stats.gpu.raster_cache_hits),
                            static_cast<jlong>(stats.completed_frame_hit),
                            static_cast<jlong>(stats.gpu.compute_blur_passes),
                            static_cast<jlong>(stats.gpu.raster_composite_pixels),
                            static_cast<jlong>(stats.gpu.raster_cache_copy_pixels),
                            static_cast<jlong>(stats.rectangular_clip_planes),
                            static_cast<jlong>(stats.mask_clip_planes),
                            static_cast<jlong>(stats.gpu.source_miss_reasons),
                            static_cast<jlong>(stats.gpu.blur_miss_reasons),
                            static_cast<jlong>(stats.gpu.stable_full_frame_planes),
                            static_cast<jlong>(stats.gpu.kernel_source_copy_pixels),
                            static_cast<jlong>(stats.gpu.kernel_source_reuses),
                            static_cast<jlong>(stats.gpu.blur_result_adoptions),
                            static_cast<jlong>(stats.native_fallback_reason),
                            static_cast<jlong>(stats.native_rendered)};
    const auto count = static_cast<jsize>(sizeof(values) / sizeof(values[0]));
    auto result = environment->NewLongArray(count);
    if (result) {
      environment->SetLongArrayRegion(result, 0, count, values);
    }
    return result;
  } catch (const std::exception& error) {
    JavaFailure(environment, error);
    return nullptr;
  }
}

extern "C" JNIEXPORT jbyteArray JNICALL Java_org_huxerui_HuxerUINativeOpticsTest_referencePixels(
    JNIEnv* environment, jclass, jbyteArray rgba, jint width, jint height, jfloatArray matrix,
    jfloatArray material, jfloatArray geometry) {
  try {
    Check(width > 0 && height > 0 && width <= 4096 && height <= 4096,
          "HuxerUI optical reference dimensions are invalid");
    const auto bytes = static_cast<std::size_t>(width) * height * 4;
    Check(rgba && matrix && material && geometry &&
              environment->GetArrayLength(rgba) == static_cast<jsize>(bytes) &&
              environment->GetArrayLength(matrix) == 9 && environment->GetArrayLength(material) == 9 &&
              environment->GetArrayLength(geometry) >= 8 && environment->GetArrayLength(geometry) <= 64 * 8 &&
              environment->GetArrayLength(geometry) % 8 == 0,
          "HuxerUI optical reference arrays are invalid");
    std::vector<std::uint8_t> pixels(bytes);
    std::array<jfloat, 9> transform{}, effect{};
    std::vector<jfloat> shape(static_cast<std::size_t>(environment->GetArrayLength(geometry)));
    environment->GetByteArrayRegion(rgba, 0, static_cast<jsize>(bytes), reinterpret_cast<jbyte*>(pixels.data()));
    if (environment->ExceptionCheck()) return nullptr;
    environment->GetFloatArrayRegion(matrix, 0, 9, transform.data());
    if (environment->ExceptionCheck()) return nullptr;
    environment->GetFloatArrayRegion(material, 0, 9, effect.data());
    if (environment->ExceptionCheck()) return nullptr;
    environment->GetFloatArrayRegion(geometry, 0, static_cast<jsize>(shape.size()), shape.data());
    if (environment->ExceptionCheck()) return nullptr;
    for (std::size_t i = 0; i < bytes; i += 4) std::swap(pixels[i], pixels[i + 2]);
    const Transform2D pixel_to_local{transform[0], transform[3], transform[1], transform[4],
                                    transform[2], transform[5]};
    std::vector<BackdropShape> shapes;
    for (std::size_t index = 0; index < shape.size(); index += 8) {
      shapes.push_back({{shape[index], shape[index + 1], shape[index + 2], shape[index + 3]},
                        {shape[index + 4], shape[index + 5], shape[index + 6], shape[index + 7]}});
    }
    const DrawBackdropCommand command{
        .shapes = std::move(shapes),
        .effect = {.blur_radius = effect[0], .refraction = effect[1], .edge_width = effect[2],
                   .highlight = effect[3], .light_direction = {effect[4], effect[5]},
                   .saturation = effect[6], .brightness = effect[7], .fusion_radius = effect[8]}};
    auto result = ApplyBackdropEffect(pixels, width, height, width * 4, pixel_to_local, command).bgra;
    for (std::size_t i = 0; i < bytes; i += 4) std::swap(result[i], result[i + 2]);
    auto output = environment->NewByteArray(static_cast<jsize>(bytes));
    if (output) environment->SetByteArrayRegion(output, 0, static_cast<jsize>(bytes),
                                               reinterpret_cast<const jbyte*>(result.data()));
    return output;
  } catch (const std::exception& error) {
    JavaFailure(environment, error);
    return nullptr;
  }
}
