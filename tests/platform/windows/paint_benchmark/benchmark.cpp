// Compile the exact baseline or working-tree renderer in a separate executable. Renaming its private classes
// avoids collisions with the framework archive; timing and readback stay entirely outside production code.
#define Win32Renderer Win32PaintBenchmarkRenderer
#define Win32TextLayout Win32PaintBenchmarkTextLayout
#include HUXERUI_BENCHMARK_RENDERER

#include <psapi.h>

#include <chrono>
#include <iomanip>
#include <iostream>

namespace huxerui::detail {

namespace {

using Clock = std::chrono::steady_clock;

double Microseconds(Clock::time_point start, Clock::time_point end) {
  return std::chrono::duration<double, std::micro>(end - start).count();
}

std::uint64_t ProcessCpuTicks() {
  FILETIME created{}, exited{}, kernel{}, user{};
  if (!GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user)) {
    throw std::runtime_error("HuxerUI benchmark could not query process CPU time");
  }
  const auto ticks = [](FILETIME time) {
    return (static_cast<std::uint64_t>(time.dwHighDateTime) << 32U) | time.dwLowDateTime;
  };
  return ticks(kernel) + ticks(user);
}

PROCESS_MEMORY_COUNTERS_EX Memory() {
  PROCESS_MEMORY_COUNTERS_EX memory{};
  memory.cb = sizeof(memory);
  if (!GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&memory), sizeof(memory))) {
    throw std::runtime_error("HuxerUI benchmark could not query process memory");
  }
  return memory;
}

struct Sample {
  double submit_us;
  double gpu_us;
  double completed_us;
};

struct DeviceQueries {
  ComPtr<ID3D11DeviceContext> context;
  ComPtr<ID3D11Query> disjoint;
  ComPtr<ID3D11Query> start;
  ComPtr<ID3D11Query> end;

  explicit DeviceQueries(ID3D11Device& device) {
    device.GetImmediateContext(context.GetAddressOf());
    D3D11_QUERY_DESC description{D3D11_QUERY_TIMESTAMP_DISJOINT, 0};
    ThrowIfFailed(device.CreateQuery(&description, disjoint.GetAddressOf()), "HuxerUI benchmark query creation failed");
    description.Query = D3D11_QUERY_TIMESTAMP;
    ThrowIfFailed(device.CreateQuery(&description, start.GetAddressOf()), "HuxerUI benchmark query creation failed");
    ThrowIfFailed(device.CreateQuery(&description, end.GetAddressOf()), "HuxerUI benchmark query creation failed");
  }

  template <class Value> Value Read(ID3D11Query& query) {
    Value value{};
    const auto deadline = Clock::now() + std::chrono::seconds(10);
    for (;;) {
      const HRESULT result = context->GetData(&query, &value, sizeof(value), D3D11_ASYNC_GETDATA_DONOTFLUSH);
      if (result == S_OK) {
        return value;
      }
      ThrowIfFailed(result, "HuxerUI benchmark GPU query failed");
      if (Clock::now() >= deadline) {
        throw std::runtime_error("HuxerUI benchmark GPU query timed out");
      }
      Sleep(1);
    }
  }
};

RenderNode Scene(std::string_view name, int frame, float width, float height) {
  RenderNode root;
  PaintContext paint(root.content, {0.0F, 0.0F, width, height});
  paint.DrawRect({0.0F, 0.0F, width, height}, Color::White());
  const bool shadows = name.starts_with("shadows") || name == "mixed";
  const bool gradients = name.starts_with("gradients") || name == "mixed";
  const bool churn = name.ends_with("churn");
  const bool animated = name.ends_with("animated") || churn || name == "mixed";
  const float phase = animated ? static_cast<float>(frame) / 64.0F : 0.0F;
  for (int index = 0; index < 96 && name != "empty"; ++index) {
    const float x = 10.0F + static_cast<float>(index % 12) * 104.0F;
    const float y = 10.0F + static_cast<float>(index / 12) * 86.0F;
    paint.PushTransform({.translate_x = x, .translate_y = y});
    const float radius = churn ? 2.0F + static_cast<float>(index) * 0.07F + phase : 4.0F;
    const Path path = Path::RoundedRect({0.0F, 0.0F, 84.0F, 60.0F}, CornerRadii{radius});
    if (shadows) {
      paint.DrawPathShadow(path, Color::Rgb(24, 32, 80, 0.2F + phase * 0.3F), {phase * 4.0F, 2.0F + phase * 2.0F},
                           12.0F);
    }
    if (gradients) {
      const int color = churn ? (index * 7 + frame * 3) % 256 : 80;
      const std::vector<GradientStop> stops{{0.0F, Color::Rgb(color, 180, 250)}, {1.0F, Color::Rgb(180, 80, 210)}};
      if (index % 2 == 0) {
        paint.FillPath(path, LinearGradient{.start = {phase, 0.0F}, .end = {1.0F, 1.0F - phase}, .stops = stops});
      } else {
        paint.FillPath(path,
                       RadialGradient{.center = {0.25F + phase * 0.5F, 0.5F}, .radius = {0.5F, 0.75F}, .stops = stops});
      }
    }
    paint.PopTransform();
  }
  paint.Finish();
  return root;
}

} // namespace

struct Win32RendererTestAccess {
  static int Run(std::string_view name, float dpi, int warmup, int sample_count) {
    Win32Renderer renderer;
    renderer.Initialize();
    auto& state = *renderer.state_;
    state.dpi_ = dpi;
    if (!state.EnsureDeviceResources()) {
      throw std::runtime_error("HuxerUI benchmark could not create the render device");
    }
    const auto properties =
        D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_TARGET,
                                D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), dpi, dpi);
    const auto pixel_width = static_cast<UINT32>(1280.0F * dpi / 96.0F);
    const auto pixel_height = static_cast<UINT32>(720.0F * dpi / 96.0F);
    ThrowIfFailed(state.device_context_->CreateBitmap(D2D1::SizeU(pixel_width, pixel_height), nullptr, 0, &properties,
                                                      state.scene_target_.GetAddressOf()),
                  "HuxerUI benchmark could not create its offscreen target");
    state.device_context_->SetTarget(state.scene_target_.Get());
    ComPtr<IDXGIDevice> dxgi;
    ComPtr<IDXGIAdapter> adapter;
    ThrowIfFailed(state.d3d_device_.As(&dxgi), "HuxerUI benchmark adapter query failed");
    ThrowIfFailed(dxgi->GetAdapter(adapter.GetAddressOf()), "HuxerUI benchmark adapter query failed");
    DXGI_ADAPTER_DESC description{};
    ThrowIfFailed(adapter->GetDesc(&description), "HuxerUI benchmark adapter description failed");
    std::wcerr << L"adapter=" << description.Description << L" vendor=" << description.VendorId << L" device="
               << description.DeviceId << L" dpi=" << dpi << L"\n";
    std::vector<RenderNode> scenes;
    for (int frame = 0; frame < 64; ++frame) {
      scenes.push_back(Scene(name, frame, 1280.0F, 720.0F));
    }
    DeviceQueries queries(*state.d3d_device_.Get());
    const auto draw = [&](int frame) -> std::optional<Sample> {
      queries.context->Begin(queries.disjoint.Get());
      queries.context->End(queries.start.Get());
      const auto started = Clock::now();
      state.device_context_->BeginDraw();
      state.device_context_->SetTransform(D2D1::Matrix3x2F::Identity());
      state.device_context_->Clear(D2D1::ColorF(D2D1::ColorF::White));
      state.RenderSceneNode(scenes[static_cast<std::size_t>(frame) % scenes.size()]);
      ThrowIfFailed(state.device_context_->EndDraw(), "HuxerUI benchmark offscreen rendering failed");
      const auto submitted = Clock::now();
      queries.context->End(queries.end.Get());
      queries.context->End(queries.disjoint.Get());
      queries.context->Flush();
      const auto timing = queries.Read<D3D11_QUERY_DATA_TIMESTAMP_DISJOINT>(*queries.disjoint.Get());
      const auto start = queries.Read<UINT64>(*queries.start.Get());
      const auto end = queries.Read<UINT64>(*queries.end.Get());
      const auto completed = Clock::now();
      if (timing.Disjoint || timing.Frequency == 0) {
        return std::nullopt;
      }
      return Sample{Microseconds(started, submitted), static_cast<double>(end - start) * 1.0e6 / timing.Frequency,
                    Microseconds(started, completed)};
    };
    for (int frame = 0; frame < warmup; ++frame) {
      draw(frame);
    }
    const auto before = Memory();
    const auto cpu_before = ProcessCpuTicks();
    std::vector<Sample> samples;
    samples.reserve(static_cast<std::size_t>(sample_count));
    int rejected = 0;
    while (samples.size() < static_cast<std::size_t>(sample_count)) {
      if (auto sample = draw(warmup + static_cast<int>(samples.size()))) {
        samples.push_back(*sample);
      } else if (++rejected > 20) {
        throw std::runtime_error("HuxerUI benchmark GPU clock remained disjoint");
      }
    }
    const double cpu_us = static_cast<double>(ProcessCpuTicks() - cpu_before) / 10.0 / sample_count;
    const auto after = Memory();
    // Readback is deliberately outside the timed region and also verifies completion and pixel equivalence.
    state.device_context_->SetTarget(nullptr);
    const auto read_properties = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_CPU_READ | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
                                                         state.scene_target_->GetPixelFormat());
    ComPtr<ID2D1Bitmap1> readback;
    ThrowIfFailed(state.device_context_->CreateBitmap(D2D1::SizeU(pixel_width, pixel_height), nullptr, 0,
                                                      &read_properties, readback.GetAddressOf()),
                  "HuxerUI benchmark readback failed");
    ThrowIfFailed(readback->CopyFromBitmap(nullptr, state.scene_target_.Get(), nullptr),
                  "HuxerUI benchmark copy failed");
    D2D1_MAPPED_RECT mapped{};
    ThrowIfFailed(readback->Map(D2D1_MAP_OPTIONS_READ, &mapped), "HuxerUI benchmark map failed");
    std::uint64_t checksum = 14695981039346656037ULL;
    for (std::size_t row = 0; row < pixel_height; ++row) {
      for (std::size_t column = 0; column < static_cast<std::size_t>(pixel_width) * 4; ++column) {
        checksum = (checksum ^ mapped.bits[row * mapped.pitch + column]) * 1099511628211ULL;
      }
    }
    ThrowIfFailed(readback->Unmap(), "HuxerUI benchmark unmap failed");
    std::cout << "sample,submit_us,gpu_us,completed_us,process_cpu_us,private_before,private_after,working_set,"
                 "checksum,rejected\n";
    std::cout << std::fixed << std::setprecision(4);
    for (std::size_t index = 0; index < samples.size(); ++index) {
      const auto& sample = samples[index];
      std::cout << index << ',' << sample.submit_us << ',' << sample.gpu_us << ',' << sample.completed_us << ','
                << cpu_us << ',' << before.PrivateUsage << ',' << after.PrivateUsage << ',' << after.WorkingSetSize
                << ',' << checksum << ',' << rejected << '\n';
    }
    renderer.Discard();
    return 0;
  }
};

} // namespace huxerui::detail

int main(int argc, char** argv) {
  if (argc != 5) {
    std::cerr << "Usage: paint_benchmark <scene> <dpi> <warmup-frames> <sample-frames>\n";
    return 2;
  }
  const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  if (FAILED(com)) {
    return 3;
  }
  int result = 1;
  try {
    const std::string name = argv[1];
    const std::vector<std::string> names{"empty",          "gradients_static", "gradients_animated", "gradients_churn",
                                         "shadows_static", "shadows_animated", "shadows_churn",      "mixed"};
    const float dpi = std::stof(argv[2]);
    const int warmup = std::stoi(argv[3]);
    const int samples = std::stoi(argv[4]);
    if (std::find(names.begin(), names.end(), name) == names.end() || !std::isfinite(dpi) || dpi < 96.0F ||
        dpi > 192.0F || warmup < 0 || samples <= 0) {
      throw std::invalid_argument("HuxerUI benchmark arguments are outside their supported ranges");
    }
    result = huxerui::detail::Win32RendererTestAccess::Run(name, dpi, warmup, samples);
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
  }
  CoUninitialize();
  return result;
}
