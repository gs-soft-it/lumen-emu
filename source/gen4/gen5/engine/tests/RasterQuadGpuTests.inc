// SPDX-License-Identifier: GPL-2.0-only
// Synthetic guest ISA only. The CPU reference executes whole guest waves with
// shared scalar state; it does not replace wave votes with quad votes by fiat.
namespace RasterQuadGpu {
constexpr auto Name = "RasterQuad";

struct ReferenceResult {
  std::array<u32, 64> value{};
  std::array<u32, 64> scalar_after{};
};

ReferenceResult ReferenceLoop(std::span<const u32> bounds, u32 partition,
                              uint64_t initial_exec) {
  ReferenceResult result;
  Require(Name, "reference partition", bounds.size() % partition == 0,
          "reference partition must cover the complete guest wave");
  for (u32 first = 0; first < bounds.size(); first += partition) {
    // One scalar counter and one EXEC mask per executed guest wave. Running
    // with partition=4 is a separate candidate execution, never the oracle.
    u32 scalar_iteration = 0;
    uint64_t exec = initial_exec;
    while (true) {
      bool any = false;
      for (u32 lane = first; lane < first + partition; ++lane) {
        const bool active = ((exec >> lane) & 1u) && scalar_iteration < bounds[lane];
        if (!active) exec &= ~(uint64_t{1} << lane);
        any |= active;
      }
      if (!any) break;
      for (u32 lane = first; lane < first + partition; ++lane)
        if ((exec >> lane) & 1u) ++result.value[lane];
      Require(Name, "reference termination", ++scalar_iteration <= 16,
              "bounded synthetic loop exceeded its independent step limit");
    }
    for (u32 lane = first; lane < first + partition; ++lane)
      result.scalar_after[lane] = scalar_iteration;
  }
  return result;
}

std::array<u32, 64> ReferenceRestoringRegion(u32 wave, u32 partition, uint64_t exec) {
  std::array<u32, 64> values{};
  for (u32 first = 0; first < wave; first += partition) {
    bool enters_region = false;
    for (u32 lane = first; lane < first + partition; ++lane)
      enters_region |= (exec >> lane) & 1u;
    if (!enters_region) continue;
    // S_MOV EXEC,savedEXEC occurs before the write, inside the voted region.
    for (u32 lane = first; lane < first + partition; ++lane) values[lane] += 9;
  }
  return values;
}

std::array<u32, 64> ReferenceDpp(std::span<const u32> input, u32 control,
                                uint64_t source_exec, uint64_t dest_exec,
                                bool fetch_inactive, u32 previous) {
  std::array<u32, 64> result{};
  for (u32 lane = 0; lane < input.size(); ++lane) {
    result[lane] = previous;
    if (!((dest_exec >> lane) & 1u)) continue;
    const u32 source = lane / 4 * 4 + ((control >> ((lane % 4) * 2)) & 3u);
    result[lane] = fetch_inactive || ((source_exec >> source) & 1u) ? input[source] : 0;
  }
  return result;
}

std::array<u32, 64> ReferenceNestedCounter(std::span<const u32> bounds, u32 partition) {
  std::array<u32, 64> result{};
  for (u32 first = 0; first < bounds.size(); first += partition) {
    // The inner shared scalar counter is dead after the outer loop, but is
    // observable by vector writes in its next enclosing region. A proof that
    // checks only the final function live-outs would incorrectly accept this.
    for (u32 outer = 0; outer < 2; ++outer) {
      u32 scalar_inner = 0;
      while (true) {
        bool any = false;
        for (u32 lane = first; lane < first + partition; ++lane)
          any |= scalar_inner < bounds[lane];
        if (!any) break;
        Require(Name, "nested reference termination", ++scalar_inner <= 16,
                "nested reference exceeded its finite bound");
      }
      for (u32 lane = first; lane < first + partition; ++lane)
        result[lane] += scalar_inner;
    }
  }
  return result;
}

struct NestedReferenceResult {
  std::array<u32, 64> updates{}, counter{}, shared_updates{};
};

NestedReferenceResult ReferenceNestedStutter(std::span<const u32> delta, u32 partition,
                                            uint64_t initial_exec) {
  NestedReferenceResult result;
  for (u32 first = 0; first < delta.size(); first += partition) {
    u32 shared_updates = 0;
    for (u32 outer = 0; outer < 2; ++outer) {
      std::array<u32, 64> limit{};
      for (u32 lane = first; lane < first + partition; ++lane)
        limit[lane] = std::min(8u, result.counter[lane] + delta[lane]);
      uint64_t exec = initial_exec;
      u32 bounded_steps = 0;
      while (true) {
        bool any = false;
        for (u32 lane = first; lane < first + partition; ++lane) {
          const bool active = ((exec >> lane) & 1u) && result.counter[lane] < limit[lane];
          if (!active) exec &= ~(uint64_t{1} << lane);
          any |= active;
        }
        if (!any) break;
        ++shared_updates; // Real unpredicated scalar state, used by the negative only.
        for (u32 lane = first; lane < first + partition; ++lane) {
          if (!((exec >> lane) & 1u)) continue;
          ++result.updates[lane];
          const bool skip = result.counter[lane] == 1;
          ++result.counter[lane];
          if (skip) result.counter[lane] = limit[lane];
        }
        Require(Name, "nested stutter termination", ++bounded_steps <= 8,
                "synthetic nested loop exceeded its integer bound");
      }
      // Restoring the original EXEC here preserves each lane's counter. It
      // feeds the next outer iteration's bound; it is not reset to zero.
    }
    for (u32 lane = first; lane < first + partition; ++lane)
      result.shared_updates[lane] = shared_updates;
  }
  return result;
}

std::array<u32, 64> ReferenceCurrentPhi(std::span<const u32> bounds, u32 partition) {
  std::array<u32, 64> a{}, b{};
  for (u32 first = 0; first < bounds.size(); first += partition) {
    u32 steps = 0;
    while (true) {
      std::array<bool, 64> active{};
      bool any = false;
      for (u32 lane = first; lane < first + partition; ++lane) {
        active[lane] = a[lane] < bounds[lane];
        any |= active[lane];
      }
      if (!any) break;
      for (u32 lane = first; lane < first + partition; ++lane) {
        if (active[lane]) ++a[lane];
        b[lane] = active[lane] ? b[lane] : a[lane];
      }
      Require(Name, "current Phi reference termination", ++steps <= 8,
              "counterexample exceeded its synthetic bound");
    }
  }
  return b;
}

std::array<u32, 64> ReferenceFutureActivation(std::span<const u32> thresholds, u32 partition) {
  std::array<u32, 64> values{};
  for (u32 first = 0; first < thresholds.size(); first += partition) {
    u32 scalar_iteration = 0;
    while (true) {
      bool any = false;
      for (u32 lane = first; lane < first + partition; ++lane)
        any |= scalar_iteration < 8 && scalar_iteration >= thresholds[lane];
      if (!any) break;
      for (u32 lane = first; lane < first + partition; ++lane)
        if (scalar_iteration < 8 && scalar_iteration >= thresholds[lane]) ++values[lane];
      ++scalar_iteration; // Not exported: its future effect is solely control.
    }
  }
  return values;
}

void CheckReference() {
  size_t comparisons = 0;
  for (u32 wave : {32u, 64u}) {
    for (u32 phase = 0; phase < 8; ++phase) {
      for (u32 mask : {0u, 1u, 3u, 5u, 10u, 15u}) {
        std::array<u32, 64> bounds{};
        uint64_t exec = 0;
        constexpr std::array limits{0u, 1u, 2u, 5u};
        for (u32 lane = 0; lane < wave; ++lane) {
          bounds[lane] = limits[((phase < 4 ? lane / 4u : lane) + phase) % limits.size()];
          // Rotate local masks independently from bounds, including completely
          // empty partitions beside partitions that keep the wave alive.
          const auto local_mask = (lane / 4u + phase) % 3u == 0 ? 0u : mask;
          if ((local_mask >> (lane % 4u)) & 1u) exec |= uint64_t{1} << lane;
        }
        const auto guest = ReferenceLoop(std::span(bounds).first(wave), wave, exec);
        const auto quads = ReferenceLoop(std::span(bounds).first(wave), 4, exec);
        for (u32 lane = 0; lane < wave; ++lane) {
          const u32 expected = (exec >> lane) & 1u ? bounds[lane] : 0u;
          Require(Name, "full-wave stutter oracle",
                  guest.value[lane] == expected && quads.value[lane] == guest.value[lane],
                  "predicated loop changed an inactive lane or its iteration count");
          ++comparisons;
        }
        const auto guest_nested = ReferenceNestedStutter(std::span(bounds).first(wave), wave, exec);
        const auto quad_nested = ReferenceNestedStutter(std::span(bounds).first(wave), 4, exec);
        Require(Name, "nested hold/skip/bound/live-out oracle",
                guest_nested.updates == quad_nested.updates &&
                    guest_nested.counter == quad_nested.counter,
                "nested partition changed predicated updates or the persistent per-lane counter");
        for (u32 lane = 0; lane < wave; ++lane)
          Require(Name, "nested persistent counter",
                  guest_nested.counter[lane] == (((exec >> lane) & 1u) ? std::min(8u, 2 * bounds[lane]) : 0u),
                  "nested counter failed to carry its bounded progress across the outer loop");
      }
    }
    std::array<u32, 64> bounds{};
    for (u32 lane = 0; lane < wave; ++lane) bounds[lane] = lane < 4 ? 1u : 5u;
    const auto all = wave == 64 ? ~uint64_t{0} : (uint64_t{1} << wave) - 1;
    const auto guest = ReferenceLoop(std::span(bounds).first(wave), wave, all);
    const auto quads = ReferenceLoop(std::span(bounds).first(wave), 4, all);
    Require(Name, "escaped scalar counter counterexample",
            guest.scalar_after[0] == 5 && quads.scalar_after[0] == 1,
            "reference failed to expose the shared SGPR live-out mismatch");
    Require(Name, "nested scalar counter counterexample",
            ReferenceNestedCounter(std::span(bounds).first(wave), wave) !=
                ReferenceNestedCounter(std::span(bounds).first(wave), 4),
            "nested scalar counter use was incorrectly treated as dead state");
    const auto guest_nested = ReferenceNestedStutter(std::span(bounds).first(wave), wave, all);
    const auto quad_nested = ReferenceNestedStutter(std::span(bounds).first(wave), 4, all);
    Require(Name, "nested shared update counter counterexample",
            guest_nested.shared_updates[0] != quad_nested.shared_updates[0],
            "unpredicated nested scalar updates must remain observable and unsupported");
    const auto guest_phi = ReferenceCurrentPhi(std::span(bounds).first(wave), wave);
    const auto quad_phi = ReferenceCurrentPhi(std::span(bounds).first(wave), 4);
    Require(Name, "current Phi identity counterexample", guest_phi[0] == 1 && quad_phi[0] == 0,
            "equal entry values must not equate distinct current loop-carried Phis");
    std::array<u32, 64> thresholds{};
    std::fill_n(thresholds.begin(), 4, 5u);
    const auto guest_future = ReferenceFutureActivation(std::span(thresholds).first(wave), wave);
    const auto quad_future = ReferenceFutureActivation(std::span(thresholds).first(wave), 4);
    Require(Name, "future activation counterexample",
            guest_future[0] == 3 && quad_future[0] == 0,
            "an unexported scalar counter can reactivate an initially empty quad");
    // If EXEC is restored inside a skipped region, an initially empty quad
    // would miss writes executed by the full wave because another quad voted.
    const auto wave_restore = ReferenceRestoringRegion(wave, wave, all & ~uint64_t{15});
    const auto quad_restore = ReferenceRestoringRegion(wave, 4, all & ~uint64_t{15});
    Require(Name, "premature EXEC restore counterexample", wave_restore != quad_restore,
            "reference failed to expose writes to reactivated lanes");
    // Row-xor 4 reads another quad. Four-lane wrapping is not an admissible
    // implementation even when ordinary quad permutations were certified.
    std::array<u32, 64> input{};
    for (u32 lane = 0; lane < wave; ++lane) input[lane] = 17 + 3 * lane;
    std::array<u32, 64> row_xor{}, quad_wrap{};
    for (u32 lane = 0; lane < wave; ++lane) {
      row_xor[lane] = input[lane ^ 4u];
      quad_wrap[lane] = input[(lane & ~3u) | ((lane ^ 4u) & 3u)];
    }
    Require(Name, "extra-quad counterexample", row_xor != quad_wrap,
            "reference failed to distinguish an out-of-quad DPP source");
    // Exercise inactive-source and destination masks independently. The GPU
    // positives use full EXEC; these references explicitly retain the semantic
    // differences that forbid extending that certificate to arbitrary masks.
    for (u32 source_mask = 0; source_mask < 16; ++source_mask) {
      for (u32 dest_mask = 0; dest_mask < 16; ++dest_mask) {
        uint64_t source_exec = 0, dest_exec = 0;
        for (u32 lane = 0; lane < wave; ++lane) {
          source_exec |= uint64_t((source_mask >> (lane % 4)) & 1u) << lane;
          dest_exec |= uint64_t((dest_mask >> (lane % 4)) & 1u) << lane;
        }
        for (u32 source : {0u, 1u, 2u, 3u}) {
          const u32 control = source * 0x55u;
          for (bool fetch_inactive : {false, true}) {
            const auto values = ReferenceDpp(std::span(input).first(wave), control,
                                             source_exec, dest_exec, fetch_inactive, 0xdeadbeefu);
            for (u32 lane = 0; lane < wave; ++lane) {
              const u32 expected = !((dest_mask >> (lane % 4)) & 1u) ? 0xdeadbeefu
                  : fetch_inactive || ((source_mask >> source) & 1u)
                      ? input[lane / 4 * 4 + source] : 0;
              Require(Name, "DPP source/destination mask oracle", values[lane] == expected,
                      "DPP lost inactive-source zeroing or destination preservation");
            }
          }
        }
      }
    }
  }
  std::printf("[host]    RasterQuad full-wave oracle: %zu lane comparisons; "
              "SGPR live-out, EXEC restore and extra-quad counterexamples preserved\n", comparisons);
}

enum class Variant { Prefix, Region, Loop, Nested, EscapedScalar, NestedEscapedScalar,
                     CurrentPhi, FutureActivation, EarlyRestore, ExtraQuad };
const char *VariantName(Variant variant) {
  switch (variant) {
  case Variant::Prefix: return "prefix";
  case Variant::Region: return "region";
  case Variant::Loop: return "loop";
  case Variant::Nested: return "nested";
  case Variant::EscapedScalar: return "escaped-scalar";
  case Variant::NestedEscapedScalar: return "nested-escaped-scalar";
  case Variant::CurrentPhi: return "current-phi";
  case Variant::FutureActivation: return "future-activation";
  case Variant::EarlyRestore: return "early-restore";
  case Variant::ExtraQuad: return "extra-quad";
  }
  return "invalid";
}

std::vector<u32> Code(Variant variant, bool bound_control) {
  std::vector<u32> code;
  for (u32 component = 0; component < 2; ++component) {
    code.push_back(EncodeVintrp(0, 16 + component, 0, component, 0));
    code.push_back(EncodeVintrp(1, 16 + component, 0, component, 1));
  }
  code.insert(code.end(), {
      EncodeSmem0(0x08, 12, 4), EncodeSmem1(0, 125),
      EncodeVop1(0x01, 8, 12),
      EncodeMimg0(0x20, 0xf), EncodeMimg1(20, 16, 4, 6)});
  const auto dpp = [&](u32 dst, u32 src, u32 control) {
    code.push_back(EncodeVop1(0x01, dst, 250));
    code.push_back(EncodeVop1Dpp(src, control) | (bound_control ? 1u << 19u : 0u));
  };
  dpp(30, 20, 0x00);
  dpp(31, 20, variant == Variant::ExtraQuad ? 0x164 : 0x55);
  dpp(32, 21, 0x00);
  dpp(33, 21, 0xaa);
  code.push_back(EncodeVop2(0x04, 34, Vgpr(31), 30));
  code.push_back(EncodeVop2(0x04, 35, Vgpr(33), 32));
  code.push_back(EncodeVop1(0x01, 10, InlineU32(0)));
  code.push_back(EncodeVop1(0x01, 11, InlineU32(0)));
  AppendVMovLiteral(&code, 13, std::bit_cast<u32>(256.f));
  code.push_back(EncodeVop2(0x08, 12, Vgpr(13), 22));
  // UNorm byte*256 lies safely in [n,n+1) for the fixture's 0/1/2/5;
  // truncation gives an exact signed-int bound without FP loop termination.
  code.push_back(EncodeVop1(0x08, 12, Vgpr(12))); // V_CVT_I32_F32
  AppendVMovLiteral(&code, 15, std::bit_cast<u32>(2.f));
  code.push_back(EncodeVop2(0x08, 18, Vgpr(15), 16));
  code.push_back(EncodeVop2(0x08, 19, Vgpr(15), 17));
  code.push_back(EncodeSop1(0x04, 4, 126)); // saved full EXEC
  const bool nested = variant == Variant::Nested || variant == Variant::NestedEscapedScalar;
  const auto sampled_update = [&] {
    code.push_back(EncodeMimg0(0x20, 0xf));
    code.push_back(EncodeMimg1(24, 18, 4, 6));
    code.push_back(EncodeVop2(0x08, 14, Vgpr(8), 25));
    code.push_back(EncodeVop2(0x03, 10, Vgpr(14), 10));
  };
  if (variant == Variant::Prefix || variant == Variant::ExtraQuad) {
    code.push_back(EncodeVop1(0x01, 10, Vgpr(8)));
  } else if (nested) {
    AppendVMovU32(&code, 40, 0); // uniform outer iteration, explicit integer vote loop
    AppendVMovU32(&code, 41, 2);
    if (variant == Variant::NestedEscapedScalar)
      code.push_back(EncodeSMovB32(15, InlineU32(0)));
    const size_t outer = code.size();
    code.push_back(EncodeSop1(0x04, 126, 4));
    code.push_back(EncodeVopc(0x81, Vgpr(40), 41));
    code.push_back(EncodeSop2(0x0f, 126, 4, 106));
    const size_t outer_exit = code.size();
    code.push_back(0);
    code.push_back(EncodeSop1(0x04, 2, 126)); // outer active mask, preserved by inner loop
    code.push_back(EncodeVop2(0x25, 36, Vgpr(11), 12));
    code.push_back(EncodeVop2(0x11, 36, InlineU32(8), 36)); // min(8, persistent i + delta)
    const size_t inner = code.size();
    code.push_back(EncodeVopc(0x81, Vgpr(11), 36));
    code.push_back(EncodeSop2(0x0f, 126, 126, 106));
    const size_t exit = code.size();
    code.push_back(0);
    sampled_update();
    if (variant == Variant::NestedEscapedScalar)
      code.push_back(EncodeSop2(0x00, 15, 15, InlineU32(1)));
    code.push_back(EncodeSop1(0x04, 6, 126)); // preserve active mask across optional skip
    code.push_back(EncodeVopc(0x82, InlineU32(1), 11));
    code.push_back(EncodeVop2(0x25, 11, InlineU32(1), 11));
    code.push_back(EncodeSop2(0x0f, 126, 126, 106));
    const size_t skip = code.size();
    code.push_back(0);
    code.push_back(EncodeVop1(0x01, 11, Vgpr(36))); // active && P: jump to invariant bound
    const size_t skip_merge = code.size();
    code[skip] = EncodeSopp(0x08, static_cast<u32>(skip_merge - skip - 1));
    code.push_back(EncodeSop1(0x04, 126, 6));
    code.push_back(EncodeSopp(0x02, static_cast<u32>(inner - code.size() - 1)));
    const size_t inner_merge = code.size();
    code[exit] = EncodeSopp(0x08, static_cast<u32>(inner_merge - exit - 1));
    code.push_back(EncodeSop1(0x04, 126, 2));
    code.push_back(EncodeVop2(0x25, 40, InlineU32(1), 40));
    code.push_back(EncodeSopp(0x02, static_cast<u32>(outer - code.size() - 1)));
    const size_t outer_merge = code.size();
    code[outer_exit] = EncodeSopp(0x08, static_cast<u32>(outer_merge - outer_exit - 1));
    code.push_back(EncodeSop1(0x04, 126, 4));
  } else if (variant == Variant::FutureActivation) {
    code.push_back(EncodeSMovB32(14, InlineU32(0)));
    AppendVMovU32(&code, 39, 8);
    const size_t header = code.size();
    code.push_back(EncodeSop1(0x04, 126, 4));
    code.push_back(EncodeVopc(0x81, 14, 39)); // shared i < 8
    code.push_back(EncodeSop1(0x04, 6, 106));
    code.push_back(EncodeVopc(0x86, 14, 12)); // shared i >= per-lane threshold
    code.push_back(EncodeSop2(0x0f, 106, 6, 106));
    code.push_back(EncodeSop2(0x0f, 126, 4, 106));
    const size_t exit = code.size();
    code.push_back(0);
    sampled_update();
    code.push_back(EncodeSop2(0x00, 14, 14, InlineU32(1)));
    code.push_back(EncodeSopp(0x02, static_cast<u32>(header - code.size() - 1)));
    const size_t merge = code.size();
    code[exit] = EncodeSopp(0x08, static_cast<u32>(merge - exit - 1));
    code.push_back(EncodeSop1(0x04, 126, 4));
  } else {
    const bool loop = variant == Variant::Loop || variant == Variant::EscapedScalar ||
        variant == Variant::CurrentPhi;
    if (variant == Variant::EscapedScalar)
      code.push_back(EncodeSMovB32(14, InlineU32(0)));
    if (variant == Variant::CurrentPhi)
      code.push_back(EncodeVop1(0x01, 37, InlineU32(0)));
    const size_t header = code.size();
    code.push_back(EncodeVopc(loop ? 0x81 : 0x85,
                              loop ? Vgpr(11) : InlineU32(0), 12));
    code.push_back(EncodeSop2(0x0f, 126, 126, 106)); // EXEC &= VCC
    const size_t exit = code.size();
    code.push_back(0);
    if (variant == Variant::EarlyRestore)
      code.push_back(EncodeSop1(0x04, 126, 4));
    // The sample is executed by the complete quad in the voted body. Its
    // register writes retain guest EXEC predication. UV*2 selects mip1, whose
    // green channel remains observable in every active iteration.
    sampled_update();
    if (loop) {
      code.push_back(EncodeVop2(0x25, 11, InlineU32(1), 11));
      if (variant == Variant::CurrentPhi) {
        code.push_back(EncodeSop1(0x04, 6, 126));
        code.push_back(EncodeSop1(0x04, 126, 4));
        // VCC still contains the current active comparison. b = active ? b : a.
        code.push_back(EncodeVop2(0x01, 37, Vgpr(11), 37));
        code.push_back(EncodeSop1(0x04, 126, 6));
      }
      if (variant == Variant::EscapedScalar)
        code.push_back(EncodeSop2(0x00, 14, 14, InlineU32(1)));
      code.push_back(EncodeSopp(0x02, static_cast<u32>(header - code.size() - 1)));
    }
    const size_t merge = code.size();
    code[exit] = EncodeSopp(0x08, static_cast<u32>(merge - exit - 1));
    code.push_back(EncodeSop1(0x04, 126, 4));
  }
  if (nested) {
    code.push_back(EncodeVop1(variant == Variant::NestedEscapedScalar ? 0x06 : 0x05,
                            9, variant == Variant::NestedEscapedScalar ? 15 : Vgpr(11)));
    AppendVMovLiteral(&code, 13, std::bit_cast<u32>(.125f));
    code.push_back(EncodeVop2(0x08, 9, Vgpr(13), 9));
  } else if (variant == Variant::EscapedScalar || variant == Variant::CurrentPhi) {
    code.push_back(EncodeVop1(variant == Variant::EscapedScalar ? 0x06 : 0x05,
                            9, variant == Variant::EscapedScalar ? 14 : Vgpr(37)));
    AppendVMovLiteral(&code, 13, std::bit_cast<u32>(.125f));
    code.push_back(EncodeVop2(0x08, 9, Vgpr(13), 9));
  } else {
    code.push_back(EncodeVop1(0x01, 9, 242));
  }
  code.push_back(EncodeExp0(0, 0xf));
  code.push_back(EncodeExp1(34, 35, 10, 9));
  AppendEnd(&code);
  return code;
}

CompiledShader Compile(u32 wave, Variant variant, bool enable_quad = true) {
  auto user_data = MakeNativeUserData(nullptr);
  user_data[8] = user_data[9] = 0;
  user_data[10] = sizeof(u32);
  user_data[11] = 3u << 28u;
  const auto texture = MakeSampledTextureData(Prospero::BufferFormat::k8_8_8_8UNorm);
  std::copy_n(texture.begin(), 8, user_data.begin() + 16);
  user_data[18] = 63u | (63u << 14u);
  user_data[19] |= DstSel(4, 5, 6, 7) | (1u << 16u); // mip levels 0..1
  user_data[24] = user_data[25] = user_data[26] = user_data[27] = 0;
  ShaderPixelInputInfo pixel{};
  pixel.wave_size = wave;
  pixel.input_num = 1;
  pixel.ps_no_perspective = true;
  ShaderRecompiler::CompileOptions options;
  options.stage = ShaderType::Pixel;
  options.wave_size = wave;
  options.input_info.pixel = &pixel;
  options.user_data = user_data;
  options.fragment_quad_control_enabled = enable_quad;
  // Both settings are exercised. For quad permutations every target is in
  // range, so bound_control cannot change the specified guest result.
  const auto code = Code(variant, wave == 64);
  auto translated = ShaderRecompiler::TranslateProgram(code, options);
  auto plan = ShaderRecompiler::IR::ExtractResourcePlan(translated.program);
  ShaderRecompiler::IR::ResourceSnapshot resources;
  ShaderRecompiler::IR::ResourceSpecialization specialization;
  const ShaderRecompiler::IR::SrtRuntime runtime{
      .user_data = options.user_data,
      .shader_base = reinterpret_cast<uint64_t>(code.data())};
  Require(Name, "resource materialization",
          ShaderRecompiler::IR::MaterializeResources(plan, runtime, resources, specialization),
          "synthetic scalar-buffer/image resources could not be materialized");
  auto compiled = ShaderRecompiler::CompileProgram(std::move(translated), options, specialization, 8u);
  ValidateSpirv(Name, compiled.spirv);
  const bool positive = variant == Variant::Prefix || variant == Variant::Region ||
      variant == Variant::Loop || variant == Variant::Nested;
  Require(Name, "bounded quad certificate",
          compiled.program.info.fragment_quad_partitioned == (positive && enable_quad),
          fmt::format("wave={} variant={} feature={} produced the wrong proof result",
                      wave, VariantName(variant), enable_quad));
  spvtools::SpirvTools tools(SPV_ENV_VULKAN_1_2);
  std::string text;
  Require(Name, "disassembly", tools.Disassemble(compiled.spirv, &text),
          "cannot inspect the final synthetic fragment module");
  if (positive && enable_quad) {
    Require(Name, "final quad-only module",
            text.find("QuadDerivativesKHR") != std::string::npos &&
                text.find("RequireFullQuadsKHR") != std::string::npos &&
                text.find("MaximallyReconvergesKHR") != std::string::npos &&
                text.find("OpGroupNonUniformQuadBroadcast") != std::string::npos &&
                text.find("OpGroupNonUniformShuffle") == std::string::npos &&
                text.find("OpGroupNonUniformBallot") == std::string::npos &&
                text.find("SubgroupLocalInvocationId") == std::string::npos,
            "certified fragment retained a wave-wide dependency or lost required execution modes");
    if (variant != Variant::Prefix)
      Require(Name, "quad vote retained",
              text.find("OpGroupNonUniformQuadAnyKHR") != std::string::npos ||
                  text.find("OpGroupNonUniformQuadAllKHR") != std::string::npos,
              "varying guest control was silently removed instead of partitioned");
    if (variant == Variant::Loop || variant == Variant::Nested)
      Require(Name, "real loop retained",
              CountText(text, "OpLoopMerge") >= (variant == Variant::Nested ? 2u : 1u),
              "bounded synthetic loop was constant-folded");
    SpirvOptionalFeatures disabled{}, enabled{};
    enabled.fragment_quad_control = true;
    Require(Name, "final module feature admission",
            AdmitSpirvModule(compiled.spirv, false, disabled) ==
                SpirvModuleAdmission::FragmentQuadControlUnavailable &&
                AdmitSpirvModule(compiled.spirv, false, enabled) == SpirvModuleAdmission::Allowed,
            "quad module must be rejected without the enabled device contract");
  } else {
    Require(Name, "uncertified module preserved",
            text.find("QuadDerivativesKHR") == std::string::npos &&
                (text.find("OpGroupNonUniformShuffle") != std::string::npos ||
                 text.find("OpGroupNonUniformBallot") != std::string::npos),
            "unsupported guest semantics were silently rewritten as quads");
    const auto &info = compiled.program.info;
    const bool consumes_subgroup = info.subgroup_ballot || info.subgroup_vote ||
        info.subgroup_barrier || info.subgroup_shuffle || info.subgroup_arithmetic ||
        info.subgroup_local_invocation_id;
    Require(Name, "uncertified native16 admission",
            SubgroupPolicy::AdmitRaster(consumes_subgroup, 16, true) ==
                SubgroupPolicy::RasterAdmission::WidthUnavailable &&
                SubgroupPolicy::AdmitRaster(consumes_subgroup, 32, true) ==
                    SubgroupPolicy::RasterAdmission::Supported,
            "uncertified guest shader must retain native16 denial and the established native32 path");
  }
  Require(Name, "live runtime inputs", compiled.program.info.buffers.size() == 1 &&
              compiled.program.info.images.size() == 1 && compiled.program.info.samplers.size() == 1 &&
              CountText(text, "OpImageSampleImplicitLod") ==
                  (variant == Variant::Prefix || variant == Variant::ExtraQuad ? 1u : 2u) &&
              text.find("OpImageSampleExplicitLod") == std::string::npos,
          "runtime buffer or sampled varying data was lost");
  std::vector<u32> packed;
  for (const auto reg : compiled.program.info.user_data_registers)
    packed.push_back(resources.user_data[reg - compiled.program.user_data_base]);
  packed.resize(compiled.program.bindings.ShaderDataDwords());
  std::printf("[host]    RasterQuad wave=%u variant=%s feature=%u certified=%u ok\n",
              wave, VariantName(variant), enable_quad, compiled.program.info.fragment_quad_partitioned);
  return {std::move(compiled.spirv), std::move(compiled.program),
          std::move(resources), std::move(packed)};
}

struct Geometry { const char *name; float x, y, extent; };
constexpr std::array geometries{
    Geometry{"full", 0, 0, 32}, Geometry{"diagonal", 0, 0, 16},
    Geometry{"single", 3, 3, 2}, Geometry{"single-x", 4, 3, 2},
    Geometry{"single-y", 3, 4, 2}, Geometry{"single-xy", 4, 4, 2},
    Geometry{"translated", 8, 4, 8}, Geometry{"border", 0, 0, 2}};

std::vector<u32> Vertices() {
  std::vector<u32> words;
  for (const auto &geometry : geometries) {
    const std::array points{std::array{geometry.x, geometry.y},
        std::array{geometry.x + geometry.extent, geometry.y},
        std::array{geometry.x, geometry.y + geometry.extent}};
    for (const auto &point : points)
      for (float value : std::array{point[0] / 8.f - 1.f, point[1] / 8.f - 1.f, 0.f, 1.f,
          (point[0] + 16.f) / 64.f, (point[1] + 16.f) / 64.f, 0.f, 1.f})
        words.push_back(std::bit_cast<u32>(value));
  }
  return words;
}

u32 Bound(u32 x, u32 y, u32 pattern) {
  constexpr std::array values{0u, 1u, 2u, 5u};
  switch (pattern) {
  case 0: return values[(x + 2 * y) % 4];
  case 1: return values[(x / 4 + y / 4) % 4];
  case 2: return 0;
  case 3: return 5;
  default: Fail(Name, "texture pattern", "unknown synthetic pattern");
  }
}

std::vector<u32> Texture(u32 pattern) {
  std::vector<u32> data(64 * 64);
  for (u32 y = 0; y < 64; ++y)
    for (u32 x = 0; x < 64; ++x)
      data[y * 64 + x] = (17u + 3 * x) | ((13u + 3 * y) << 8u) |
          (Bound(x, y, pattern) << 16u) | 0xff000000u;
  return data;
}

void Verify(const std::vector<u32> &pixels, const Geometry &geometry, u32 wave,
            Variant variant, u32 pattern, float step) {
  constexpr u32 width = PrimitiveContract::Width, height = PrimitiveContract::Height;
  Require(Name, "readback size", pixels.size() == width * height,
          "quad fragment readback did not cover the complete RGBA8 target");
  constexpr std::array<u32, 4> red{65, 68, 65, 68}, green{61, 61, 64, 64};
  const auto red0 = ReferenceDpp(red, 0x00, 15, 15, false, 0);
  const auto red1 = ReferenceDpp(red, 0x55, 15, 15, false, 0);
  const auto green0 = ReferenceDpp(green, 0x00, 15, 15, false, 0);
  const auto green2 = ReferenceDpp(green, 0xaa, 15, 15, false, 0);
  const u32 red_difference = red1[0] - red0[0], green_difference = green2[0] - green0[0];
  std::array<u32, width * height> guest_iterations{};
  std::array<u32, width * height> guest_counter{};
  // This is a reference ordering, not a claim about physical scheduling.
  // Stutter equivalence above proves the output independent of neighbouring
  // lanes' bounds; full-wave execution remains the expected-value producer.
  for (u32 first = 0; first < guest_iterations.size(); first += wave) {
    std::array<u32, 64> bounds{};
    for (u32 lane = 0; lane < wave; ++lane) {
      const u32 pixel = first + lane;
      const auto bound = Bound(pixel % width + 16, pixel / width + 16, pattern);
      bounds[lane] = variant == Variant::Prefix ? 1u
          : variant == Variant::Region ? u32(bound != 0) : bound;
    }
    const auto exec = wave == 64 ? ~uint64_t{0} : (uint64_t{1} << wave) - 1;
    if (variant == Variant::Nested) {
      const auto result = ReferenceNestedStutter(std::span(bounds).first(wave), wave, exec);
      std::copy_n(result.updates.begin(), wave, guest_iterations.begin() + first);
      std::copy_n(result.counter.begin(), wave, guest_counter.begin() + first);
    } else {
      const auto result = ReferenceLoop(std::span(bounds).first(wave), wave, exec);
      std::copy_n(result.value.begin(), wave, guest_iterations.begin() + first);
    }
  }
  u32 covered_count = 0;
  for (u32 y = 0; y < height; ++y) {
    for (u32 x = 0; x < width; ++x) {
      const float px = float(x) + .5f, py = float(y) + .5f;
      const bool covered = px >= geometry.x && py >= geometry.y &&
          px + py < geometry.x + geometry.y + geometry.extent;
      const u32 iterations = guest_iterations[y * width + x];
      const float sample_scale = variant == Variant::Prefix ? 255.f : 128.f;
      const u32 blue = static_cast<u32>(std::lround(float(iterations) * step * sample_scale));
      const u32 alpha = variant == Variant::Nested
          ? static_cast<u32>(std::lround(float(guest_counter[y * width + x]) * .125f * 255.f)) : 255u;
      // A linear texture gradient gives DPP55.R-DPP00.R == 3/255 and
      // DPPaa.G-DPP00.G == 3/255 regardless of the quad's framebuffer origin.
      const u32 expected = covered ? (alpha << 24u) | red_difference |
          (green_difference << 8u) | (blue << 16u) : 0u;
      covered_count += covered;
      if (pixels[y * width + x] != expected)
        Fail(Name, "quad sources/helpers/varying control/loop/coverage",
             fmt::format("wave={} variant={} pattern={} step={} geometry={} pixel=({}, {}) "
                         "actual=0x{:08x} expected=0x{:08x}", wave, VariantName(variant), pattern,
                         step, geometry.name, x, y, pixels[y * width + x], expected));
    }
  }
  std::printf("[gpu]     RasterQuad wave=%u variant=%s pattern=%u step=%g "
              "geometry=%s covered=%u ok\n", wave, VariantName(variant), pattern, step,
              geometry.name, covered_count);
}
} // namespace RasterQuadGpu

void CheckRasterQuadGpu(bool host_only) {
  using namespace RasterQuadGpu;
  CheckReference();
  std::unique_ptr<VulkanHarness> vulkan;
  if (!host_only) vulkan = std::make_unique<VulkanHarness>(VulkanHarness::Profile::RasterQuad);
  constexpr std::array variants{Variant::Prefix, Variant::Region, Variant::Loop, Variant::Nested};
  std::vector<CompiledShader> fragments;
  for (u32 wave : {32u, 64u}) {
    for (auto variant : variants) fragments.push_back(Compile(wave, variant));
    for (auto variant : {Variant::EscapedScalar, Variant::NestedEscapedScalar,
                         Variant::CurrentPhi, Variant::FutureActivation,
                         Variant::EarlyRestore, Variant::ExtraQuad})
      (void)Compile(wave, variant);
    (void)Compile(wave, Variant::Loop, false);
  }
  if (host_only) {
    std::printf("RasterQuad: CPU oracle, 8 positive and 14 negative/disabled target modules "
                "compiled and SPIR-V validated; GPU not run\n");
    return;
  }
  // Match production preprocessing before creating any native fragment module.
  // Even modules without barycentrics must retain the enabled quad contract.
  for (auto &fragment : fragments) {
    auto prepared = BarycentricFallback::PrepareFragment(fragment.spirv, vulkan->BarycentricLimits());
    Require(Name, "barycentric preprocessing", prepared.status == BarycentricFallback::Status::NotNeeded &&
        prepared.plan.spirv == fragment.spirv, "ordinary quad fragment was rejected or changed");
    fragment.spirv = std::move(prepared.plan.spirv);
  }
  const auto vertex = AssembleNativeMetalContract(PrimitiveContractSpv::Vertex);
  const auto vertices = Vertices();
  const auto sampler = vulkan->CreateSampler(Name, 2, vk::Filter::eNearest);
  std::vector<PrimitiveContractDraw> draws;
  for (u32 geometry = 0; geometry < geometries.size(); ++geometry) {
    auto draw = PrimitiveContract::Draw();
    draw.first_vertex = geometry * 3;
    draws.push_back(draw);
  }
  size_t readbacks = 0;
  for (u32 pattern = 0; pattern < 4; ++pattern) {
    const std::vector mips{Texture(pattern), std::vector<u32>(32 * 32, 0xffc08040u)};
    auto texture = vulkan->CreateImageMips(Name, 64, 64, vk::Format::eR8G8B8A8Unorm,
        vk::ImageUsageFlagBits::eSampled, mips, 1, vk::ImageLayout::eShaderReadOnlyOptimal,
        vk::ImageType::e2D, vk::ImageViewType::e2D, 1);
    for (u32 shader = 0; shader < fragments.size(); ++shader) {
      const u32 wave = shader < variants.size() ? 32 : 64;
      const auto variant = variants[shader % variants.size()];
      if (variant == Variant::Prefix && pattern != 0) continue;
      for (float step : {.0625f, .125f}) {
        const std::array constants{std::bit_cast<u32>(step)};
        const auto pixels = vulkan->RenderPrimitiveContract(Name, vertex, {}, {},
            fragments[shader].spirv, 0, vertices, {}, draws, vk::PrimitiveTopology::eTriangleList,
            vk::Format::eR8G8B8A8Unorm, &fragments[shader], constants, &texture, sampler);
        Require(Name, "draw completion", pixels.size() == geometries.size(),
                "not every quad geometry produced a readback");
        for (u32 geometry = 0; geometry < geometries.size(); ++geometry)
          RasterQuadGpu::Verify(pixels[geometry], geometries[geometry], wave, variant, pattern, step);
        readbacks += pixels.size();
      }
    }
    vulkan->DestroyImage(&texture);
  }
  vulkan->Device().destroySampler(sampler);
  std::printf("RasterQuad: %zu GPU draw/readbacks passed for guest wave32/64\n", readbacks);
}
