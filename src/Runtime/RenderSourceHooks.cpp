#include "RenderSourceHooks.h"

#include "RenderSourceRegistry.h"

#include <xbyak/xbyak.h>

#include <array>
#include <atomic>
#include <bit>
#include <cstdint>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <string>

namespace Runtime
{
    namespace
    {
        enum class Gpr : std::uint8_t
        {
            kRax = 0,
            kRcx = 1,
            kRdx = 2,
            kRbx = 3,
            kRsp = 4,
            kRbp = 5,
            kRsi = 6,
            kRdi = 7,
            kR8 = 8,
            kR9 = 9,
            kR10 = 10,
            kR11 = 11,
            kR12 = 12,
            kR13 = 13,
            kR14 = 14,
            kR15 = 15
        };

        struct InlineBaseReadSite
        {
            std::string_view name;
            REL::ID functionID;
            std::ptrdiff_t offset;
            std::array<std::uint8_t, 7> expectedInstruction;
            Gpr actorRegister;
            Gpr resultRegister;
        };

        // 1.16.244: direct actor->base read that feeds the appearance builder or its FaceDB work graph.
        constexpr std::array<InlineBaseReadSite, 15> kBaseReadSites{
            InlineBaseReadSite{ "ActorAppearanceBuilder.initialBase", REL::ID{ 102205 }, 0x1A7, { 0x49, 0x8B, 0xBE, 0x98, 0x00, 0x00, 0x00 }, Gpr::kR14, Gpr::kRdi },
            InlineBaseReadSite{ "ActorAppearanceBuilder.faceResourceBase", REL::ID{ 102205 }, 0x270, { 0x4D, 0x8B, 0x86, 0x98, 0x00, 0x00, 0x00 }, Gpr::kR14, Gpr::kR8 },
            InlineBaseReadSite{ "ActorAppearanceBuilder.geometryBase", REL::ID{ 102205 }, 0x8A2, { 0x49, 0x8B, 0x8E, 0x98, 0x00, 0x00, 0x00 }, Gpr::kR14, Gpr::kRcx },
            InlineBaseReadSite{ "ActorAppearanceBuilder.changeBase", REL::ID{ 102205 }, 0xF42, { 0x49, 0x8B, 0x8E, 0x98, 0x00, 0x00, 0x00 }, Gpr::kR14, Gpr::kRcx },
            InlineBaseReadSite{ "FaceCustomizationTextures.base", REL::ID{ 40889 }, 0x30, { 0x48, 0x8B, 0xB1, 0x98, 0x00, 0x00, 0x00 }, Gpr::kRcx, Gpr::kRsi },
            InlineBaseReadSite{ "EyeCustomization.base", REL::ID{ 40892 }, 0x61, { 0x4D, 0x8B, 0xAD, 0x98, 0x00, 0x00, 0x00 }, Gpr::kR13, Gpr::kR13 },
            InlineBaseReadSite{ "HairCustomization.base", REL::ID{ 40901 }, 0x101, { 0x48, 0x8B, 0x88, 0x98, 0x00, 0x00, 0x00 }, Gpr::kRax, Gpr::kRcx },
            InlineBaseReadSite{ "TeethCustomization.base", REL::ID{ 40904 }, 0xF7, { 0x48, 0x8B, 0x88, 0x98, 0x00, 0x00, 0x00 }, Gpr::kRax, Gpr::kRcx },
            InlineBaseReadSite{ "JewelryCustomization.base", REL::ID{ 40908 }, 0xF7, { 0x48, 0x8B, 0x88, 0x98, 0x00, 0x00, 0x00 }, Gpr::kRax, Gpr::kRcx },
            InlineBaseReadSite{ "ReplaceHeadPartPostprocess.base", REL::ID{ 40916 }, 0xBE, { 0x48, 0x8B, 0x86, 0x98, 0x00, 0x00, 0x00 }, Gpr::kRsi, Gpr::kRax },
            InlineBaseReadSite{ "ReplaceHeadPartGraph.raceBase", REL::ID{ 40923 }, 0x6FB, { 0x48, 0x8B, 0xBB, 0x98, 0x00, 0x00, 0x00 }, Gpr::kRbx, Gpr::kRdi },
            InlineBaseReadSite{ "ReplaceHeadPartGraph.customizationBase", REL::ID{ 40923 }, 0x7F9, { 0x48, 0x8B, 0xB1, 0x98, 0x00, 0x00, 0x00 }, Gpr::kRcx, Gpr::kRsi },
            InlineBaseReadSite{ "AttachHeadControl.base", REL::ID{ 40926 }, 0x16, { 0x48, 0x8B, 0xA8, 0x98, 0x00, 0x00, 0x00 }, Gpr::kRax, Gpr::kRbp },
            InlineBaseReadSite{ "AttachHeadCallback.base", REL::ID{ 40927 }, 0x4A, { 0x49, 0x8B, 0xB8, 0x98, 0x00, 0x00, 0x00 }, Gpr::kR8, Gpr::kRdi },
            InlineBaseReadSite{ "FaceResourceControl.rootFaceBase", REL::ID{ 40933 }, 0x14, { 0x48, 0x8B, 0xB8, 0x98, 0x00, 0x00, 0x00 }, Gpr::kRax, Gpr::kRdi }
        };

        constexpr std::size_t kMaxThunkSize = 512;
        constexpr std::size_t kScratchRegisterCount = 4;
        constexpr std::array<Gpr, kScratchRegisterCount + 1> kScratchCandidates{
            Gpr::kRax,
            Gpr::kRcx,
            Gpr::kRdx,
            Gpr::kR10,
            Gpr::kR11
        };

        struct GeneratedThunk
        {
            std::uintptr_t address{ 0 };
            std::size_t size{ 0 };
        };

        struct PreparedPatch
        {
            std::uintptr_t address{ 0 };
            std::array<std::uint8_t, 7> bytes{};
        };

        std::atomic<bool> g_installed{ false };

        std::string BytesAt(const std::uintptr_t a_address, const std::size_t a_count)
        {
            constexpr std::string_view digits{ "0123456789ABCDEF" };
            const auto* bytes = reinterpret_cast<const std::uint8_t*>(a_address);
            std::string result;
            result.reserve(a_count * 3);
            for (std::size_t i = 0; i < a_count; ++i) {
                if (i != 0) {
                    result.push_back(' ');
                }
                result.push_back(digits[bytes[i] >> 4]);
                result.push_back(digits[bytes[i] & 0xF]);
            }
            return result;
        }

        bool PreflightSites(std::array<std::uintptr_t, kBaseReadSites.size()>& a_addresses)
        {
            bool valid = true;
            for (std::size_t i = 0; i < kBaseReadSites.size(); ++i) {
                const auto& site = kBaseReadSites[i];
                const REL::Relocation<std::uintptr_t> location{ site.functionID, site.offset };
                a_addresses[i] = location.address();
                if (std::memcmp(reinterpret_cast<const void*>(location.address()), site.expectedInstruction.data(), site.expectedInstruction.size()) != 0) {
                    REX::CRITICAL("[RenderSourceHooks] byte gate failed at '{}' (Address Library ID {} + 0x{:X}); expected canonical TESNPC load, found [{}]",
                        site.name, site.functionID.id(), site.offset, BytesAt(location.address(), site.expectedInstruction.size()));
                    valid = false;
                }
            }
            return valid;
        }

        constexpr bool IsPowerOfTwo(const std::size_t a_value) noexcept
        {
            return a_value != 0 && (a_value & (a_value - 1)) == 0;
        }

        void ValidateReadView(const RenderSourceRegistryReadView& a_view)
        {
            if (!a_view.slots || !IsPowerOfTwo(a_view.capacity) || !IsPowerOfTwo(a_view.slotSize) || a_view.slotSize < sizeof(RE::TESNPC*)) {
                throw std::runtime_error("render-source registry has an invalid generated-reader layout");
            }
            if (a_view.capacity > std::numeric_limits<std::size_t>::max() / a_view.slotSize) {
                throw std::runtime_error("render-source registry byte span overflowed");
            }

            const auto byteSpan = a_view.capacity * a_view.slotSize;
            if (!IsPowerOfTwo(byteSpan) || byteSpan - 1 > static_cast<std::size_t>(std::numeric_limits<std::int32_t>::max())) {
                throw std::runtime_error("render-source registry byte span cannot be encoded by the generated reader");
            }
            if (a_view.canonicalOffset > a_view.slotSize - sizeof(RE::TESNPC*) ||  a_view.sourceOffset > a_view.slotSize - sizeof(RE::TESNPC*) ||
                a_view.canonicalOffset > static_cast<std::size_t>(std::numeric_limits<std::int32_t>::max()) ||  a_view.sourceOffset > static_cast<std::size_t>(std::numeric_limits<std::int32_t>::max()) ||
                a_view.canonicalOffset % alignof(RE::TESNPC*) != 0 || a_view.sourceOffset % alignof(RE::TESNPC*) != 0 || reinterpret_cast<std::uintptr_t>(a_view.slots) % alignof(RE::TESNPC*) != 0) {
                throw std::runtime_error("render-source registry pointer offsets are invalid");
            }
            if (a_view.pointerShift >= 64 || a_view.foldShift >= 64 || a_view.hashMultiplier == 0) {
                throw std::runtime_error("render-source registry hash parameters are invalid");
            }
        }

        std::array<Gpr, kScratchRegisterCount> SelectScratchRegisters(const Gpr a_result)
        {
            std::array<Gpr, kScratchRegisterCount> selected{};
            std::size_t count = 0;
            for (const auto candidate : kScratchCandidates) {
                if (candidate != a_result) {
                    selected[count++] = candidate;
                    if (count == selected.size()) {
                        return selected;
                    }
                }
            }
            throw std::runtime_error("could not select render-source thunk scratch registers");
        }

        class BaseReadThunk final : public Xbyak::CodeGenerator
        {
        public:
            BaseReadThunk(const InlineBaseReadSite& a_site, const RenderSourceRegistryReadView& a_view) : Xbyak::CodeGenerator(kMaxThunkSize)
            {
                if (a_site.actorRegister == Gpr::kRsp || a_site.resultRegister == Gpr::kRsp) {
                    throw std::runtime_error("render-source thunk cannot use RSP as an actor or result register");
                }

                const Xbyak::Reg64 actor{ static_cast<int>(a_site.actorRegister) };
                const Xbyak::Reg64 result{ static_cast<int>(a_site.resultRegister) };
                const auto scratchRegisters = SelectScratchRegisters(a_site.resultRegister);
                const Xbyak::Reg64 byteOffset{ static_cast<int>(scratchRegisters[0]) };
                const Xbyak::Reg64 table{ static_cast<int>(scratchRegisters[1]) };
                const Xbyak::Reg64 loaded{ static_cast<int>(scratchRegisters[2]) };
                const Xbyak::Reg64 remaining{ static_cast<int>(scratchRegisters[3]) };
                const auto byteSpan = a_view.capacity * a_view.slotSize;
                const auto slotShift = static_cast<std::uint8_t>(std::countr_zero(a_view.slotSize));

                Xbyak::Label probe;
                Xbyak::Label found;
                Xbyak::Label done;

                pushfq();
                for (const auto scratch : scratchRegisters) {
                    push(Xbyak::Reg64{ static_cast<int>(scratch) });
                }

                // Preserve the exact contract of the replaced MOV: only the result register changes, and a null canonical pointer remains null.
                mov(result, ptr[actor + 0x98]);
                test(result, result);
                jz(done, T_NEAR);

                // Mirror StartIndex without crossing the C++ ABI. The registry is immutable after publication, and aligned x86-64 loads provide the acquire ordering required after observing the canonical key.
                mov(byteOffset, result);
                shr(byteOffset, a_view.pointerShift);
                mov(loaded, byteOffset);
                shr(loaded, a_view.foldShift);
                xor_(byteOffset, loaded);
                mov(loaded, a_view.hashMultiplier);
                imul(byteOffset, loaded);
                mov(loaded, byteOffset);
                shr(loaded, a_view.foldShift);
                xor_(byteOffset, loaded);
                and_(byteOffset, static_cast<std::uint32_t>(a_view.capacity - 1));
                shl(byteOffset, slotShift);

                mov(table, reinterpret_cast<std::uintptr_t>(a_view.slots));
                mov(remaining, static_cast<std::uint64_t>(a_view.capacity));

                L(probe);
                mov(loaded, ptr[table + byteOffset + static_cast<int>(a_view.canonicalOffset)]);
                cmp(loaded, result);
                je(found, T_NEAR);
                test(loaded, loaded);
                jz(done, T_NEAR);

                add(byteOffset, static_cast<std::uint32_t>(a_view.slotSize));
                and_(byteOffset, static_cast<std::uint32_t>(byteSpan - 1));
                dec(remaining);
                jnz(probe);
                jmp(done, T_NEAR);

                L(found);
                mov(loaded, ptr[table + byteOffset + static_cast<int>(a_view.sourceOffset)]);
                test(loaded, loaded);
                cmovnz(result, loaded);

                L(done);
                for (auto it = scratchRegisters.rbegin(); it != scratchRegisters.rend(); ++it) {
                    pop(Xbyak::Reg64{ static_cast<int>(*it) });
                }
                popfq();
                ret();
                ready();

                if (getSize() > kMaxThunkSize) {
                    throw std::runtime_error("base-read thunk exceeded its generation limit");
                }
            }
        };

         GeneratedThunk GenerateThunk(REL::Trampoline& a_trampoline, const InlineBaseReadSite& a_site, const RenderSourceRegistryReadView& a_view)
        {
            BaseReadThunk thunk{ a_site, a_view };
            const auto size = thunk.getSize();
            auto* memory = a_trampoline.allocate(size);
            std::memcpy(memory, thunk.getCode(), size);
            return GeneratedThunk{
                .address = reinterpret_cast<std::uintptr_t>(memory),
                .size = size
            };
        }
    }

    bool InstallRenderSourceHooks() noexcept
    {
        if (g_installed.load(std::memory_order_acquire)) {
            return true;
        }

        try {
            std::array<std::uintptr_t, kBaseReadSites.size()> addresses{};
            if (!PreflightSites(addresses)) {
                REX::CRITICAL("[RenderSourceHooks] one or more appearance read sites did not match; no engine code was patched");
                return false;
            }

            const auto readView = GetRenderSourceRegistryReadView();
            ValidateReadView(readView);

            auto& trampoline = REL::GetTrampoline();
            std::array<PreparedPatch, kBaseReadSites.size()> patches{};
            std::size_t generatedBytes = 0;
            for (std::size_t i = 0; i < kBaseReadSites.size(); ++i) {
                const auto thunk = GenerateThunk(trampoline, kBaseReadSites[i], readView);
                generatedBytes += thunk.size;

                auto& patch = patches[i];
                patch.address = addresses[i];
                const auto displacement64 = static_cast<std::int64_t>(thunk.address) - static_cast<std::int64_t>(patch.address + 5);
                if (displacement64 < std::numeric_limits<std::int32_t>::min() || displacement64 > std::numeric_limits<std::int32_t>::max()) {
                    REX::CRITICAL("[RenderSourceHooks] trampoline for '{}' is outside rel32 range; no engine code was patched", kBaseReadSites[i].name);
                    return false;
                }

                patch.bytes[0] = 0xE8;
                const auto displacement = static_cast<std::int32_t>(displacement64);
                std::memcpy(patch.bytes.data() + 1, &displacement, sizeof(displacement));
                patch.bytes[5] = REL::NOP;
                patch.bytes[6] = REL::NOP;
            }

            for (const auto& patch : patches) {
                REL::Relocation<std::uintptr_t>{ patch.address }.write(patch.bytes.data(), patch.bytes.size());
            }

            g_installed.store(true, std::memory_order_release);
            REX::INFO("[RenderSourceHooks] installed {} NPC read redirects ({} generated bytes)", kBaseReadSites.size(), generatedBytes);
            return true;
        } catch (const std::exception& error) {
            REX::CRITICAL("[RenderSourceHooks] installation threw before completion: {}", error.what());
        } catch (...) {
            REX::CRITICAL("[RenderSourceHooks] installation threw an unknown exception before completion");
        }
        return false;
    }
}
