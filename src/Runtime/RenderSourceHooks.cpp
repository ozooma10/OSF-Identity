#include "RenderSourceHooks.h"

#include "FaceTextureCompositor.h"
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
        constexpr std::array<InlineBaseReadSite, 16> kBaseReadSites{
            InlineBaseReadSite{"ActorAppearanceBuilder.initialBase", REL::ID{102205}, 0x1A7, {0x49, 0x8B, 0xBE, 0x98, 0x00, 0x00, 0x00}, Gpr::kR14, Gpr::kRdi},
            InlineBaseReadSite{"ActorAppearanceBuilder.faceResourceBase", REL::ID{102205}, 0x270, {0x4D, 0x8B, 0x86, 0x98, 0x00, 0x00, 0x00}, Gpr::kR14, Gpr::kR8},
            InlineBaseReadSite{"ActorAppearanceBuilder.geometryBase", REL::ID{102205}, 0x8A2, {0x49, 0x8B, 0x8E, 0x98, 0x00, 0x00, 0x00}, Gpr::kR14, Gpr::kRcx},
            InlineBaseReadSite{"ActorAppearanceBuilder.changeBase", REL::ID{102205}, 0xF42, {0x49, 0x8B, 0x8E, 0x98, 0x00, 0x00, 0x00}, Gpr::kR14, Gpr::kRcx},
            InlineBaseReadSite{"QueuedActorFaceResource.base", REL::ID{45904}, 0x83, {0x48, 0x8B, 0x9F, 0x98, 0x00, 0x00, 0x00}, Gpr::kRdi, Gpr::kRbx},
            InlineBaseReadSite{"FaceCustomizationTextures.base", REL::ID{40889}, 0x30, {0x48, 0x8B, 0xB1, 0x98, 0x00, 0x00, 0x00}, Gpr::kRcx, Gpr::kRsi},
            InlineBaseReadSite{"EyeCustomization.base", REL::ID{40892}, 0x61, {0x4D, 0x8B, 0xAD, 0x98, 0x00, 0x00, 0x00}, Gpr::kR13, Gpr::kR13},
            InlineBaseReadSite{"HairCustomization.base", REL::ID{40901}, 0x101, {0x48, 0x8B, 0x88, 0x98, 0x00, 0x00, 0x00}, Gpr::kRax, Gpr::kRcx},
            InlineBaseReadSite{"TeethCustomization.base", REL::ID{40904}, 0xF7, {0x48, 0x8B, 0x88, 0x98, 0x00, 0x00, 0x00}, Gpr::kRax, Gpr::kRcx},
            InlineBaseReadSite{"JewelryCustomization.base", REL::ID{40908}, 0xF7, {0x48, 0x8B, 0x88, 0x98, 0x00, 0x00, 0x00}, Gpr::kRax, Gpr::kRcx},
            InlineBaseReadSite{"ReplaceHeadPartPostprocess.base", REL::ID{40916}, 0xBE, {0x48, 0x8B, 0x86, 0x98, 0x00, 0x00, 0x00}, Gpr::kRsi, Gpr::kRax},
            InlineBaseReadSite{"ReplaceHeadPartGraph.raceBase", REL::ID{40923}, 0x6FB, {0x48, 0x8B, 0xBB, 0x98, 0x00, 0x00, 0x00}, Gpr::kRbx, Gpr::kRdi},
            InlineBaseReadSite{"ReplaceHeadPartGraph.customizationBase", REL::ID{40923}, 0x7F9, {0x48, 0x8B, 0xB1, 0x98, 0x00, 0x00, 0x00}, Gpr::kRcx, Gpr::kRsi},
            InlineBaseReadSite{"AttachHeadControl.base", REL::ID{40926}, 0x16, {0x48, 0x8B, 0xA8, 0x98, 0x00, 0x00, 0x00}, Gpr::kRax, Gpr::kRbp},
            InlineBaseReadSite{"AttachHeadCallback.base", REL::ID{40927}, 0x4A, {0x49, 0x8B, 0xB8, 0x98, 0x00, 0x00, 0x00}, Gpr::kR8, Gpr::kRdi},
            InlineBaseReadSite{"FaceResourceControl.rootFaceBase", REL::ID{40933}, 0x14, {0x48, 0x8B, 0xB8, 0x98, 0x00, 0x00, 0x00}, Gpr::kRax, Gpr::kRdi}};

        enum class TextureIdentityKind : std::uint8_t
        {
            kFormIDThenLookup,
            kOwnerFileLookup,
            kMaskedFormID,
            kCompositeOwnerIndex,
            kCompositeMaskedFormID
        };

        struct InlineTextureIdentitySite
        {
            std::string_view name;
            REL::ID functionID;
            std::ptrdiff_t offset;
            std::array<std::uint8_t, 12> expectedInstructions;
            std::size_t instructionSize;
            TextureIdentityKind kind;
            REL::ID tailTarget;
        };

        // 1.16.244: FaceDB uses these identity reads only to derive the generated face-texture path. 
        // Resolve the published source back to its canonical base here without giving the FormID-0 source a global alias.
        constexpr std::array<InlineTextureIdentitySite, 5> kTextureIdentitySites{
            InlineTextureIdentitySite{"FaceTexturePath.emptyTintFormID", REL::ID{40886}, 0x107, {0x8B, 0x56, 0x28, 0xE8, 0x51, 0x4F, 0x1A, 0x00}, 8, TextureIdentityKind::kFormIDThenLookup, REL::ID{1015183}},
            InlineTextureIdentitySite{"FaceTexturePath.ownerFile", REL::ID{40886}, 0x1CC, {0x48, 0x8B, 0xCE, 0xE8, 0xCC, 0xF8, 0x1C, 0x00}, 8, TextureIdentityKind::kOwnerFileLookup, REL::ID{47437}},
            InlineTextureIdentitySite{"FaceTexturePath.maskedFormID", REL::ID{40886}, 0x209, {0x8B, 0x56, 0x28, 0x8B, 0xDA, 0x23, 0xD8}, 7, TextureIdentityKind::kMaskedFormID, REL::ID{0}},
            InlineTextureIdentitySite{"FaceTextureComposite.ownerIndex", REL::ID{69597}, 0x112, {0x0F, 0xB7, 0x56, 0x30, 0x41, 0xBA, 0xFF, 0xFF, 0x00, 0x00}, 10, TextureIdentityKind::kCompositeOwnerIndex, REL::ID{0}},
            InlineTextureIdentitySite{"FaceTextureComposite.maskedFormID", REL::ID{69597}, 0x191, {0x8B, 0x7E, 0x28, 0x23, 0xF8}, 5, TextureIdentityKind::kCompositeMaskedFormID, REL::ID{0}}};

        constexpr std::size_t kMaxThunkSize = 512;
        constexpr std::size_t kMaxPatchSize = 12;
        constexpr std::size_t kScratchRegisterCount = 4;
        constexpr std::array<Gpr, kScratchRegisterCount + 1> kScratchCandidates{
            Gpr::kRax,
            Gpr::kRcx,
            Gpr::kRdx,
            Gpr::kR10,
            Gpr::kR11};

        struct GeneratedThunk
        {
            std::uintptr_t address{0};
            std::size_t size{0};
        };

        struct PreparedPatch
        {
            std::uintptr_t address{0};
            std::array<std::uint8_t, kMaxPatchSize> bytes{};
            std::size_t size{0};
        };

        struct HookTelemetry
        {
            std::atomic<std::uint32_t> lastBaseFormID{0};
            std::atomic<std::uint64_t> hitCount{0};
            std::atomic<std::uint32_t> reportedBaseFormID{0};
            std::atomic<std::uint64_t> reportedHitCount{0};
        };

        struct GeometryLookupTelemetry
        {
            std::atomic<std::uint32_t> lastBaseFormID{0};
            std::atomic<std::uintptr_t> lastResourceAddress{0};
            std::atomic<std::uint32_t> lastHeadPartFormID{0};
            std::atomic<std::uintptr_t> lastEntryAddress{0};
            std::atomic<std::uint64_t> lookupCount{0};
            std::atomic<std::uint64_t> missCount{0};
            std::atomic<std::uint64_t> reportedLookupCount{0};
        };

        std::atomic<bool> g_installed{false};
        std::array<HookTelemetry, kBaseReadSites.size()> g_hookTelemetry{};
        GeometryLookupTelemetry g_geometryLookupTelemetry{};

        // 1.16.244: FUN_140D938A0 has already filtered the detached source's
        // head parts to types 3/11 when it asks the current FaceDB resource for
        // the corresponding geometry entry.
        constexpr REL::ID kGeometryAttachmentID{69635};
        constexpr std::ptrdiff_t kGeometryLookupOffset{0x2C2};
        constexpr std::array<std::uint8_t, 6> kGeometryLookupInstruction{0xFF, 0x90, 0xF0, 0x01, 0x00, 0x00};

        std::string BytesAt(const std::uintptr_t a_address, const std::size_t a_count)
        {
            constexpr std::string_view digits{"0123456789ABCDEF"};
            const auto *bytes = reinterpret_cast<const std::uint8_t *>(a_address);
            std::string result;
            result.reserve(a_count * 3);
            for (std::size_t i = 0; i < a_count; ++i)
            {
                if (i != 0)
                {
                    result.push_back(' ');
                }
                result.push_back(digits[bytes[i] >> 4]);
                result.push_back(digits[bytes[i] & 0xF]);
            }
            return result;
        }

        bool PreflightSites(std::array<std::uintptr_t, kBaseReadSites.size()> &a_addresses)
        {
            bool valid = true;
            for (std::size_t i = 0; i < kBaseReadSites.size(); ++i)
            {
                const auto &site = kBaseReadSites[i];
                const REL::Relocation<std::uintptr_t> location{site.functionID, site.offset};
                a_addresses[i] = location.address();
                if (std::memcmp(reinterpret_cast<const void *>(location.address()), site.expectedInstruction.data(), site.expectedInstruction.size()) != 0)
                {
                    REX::CRITICAL("[RenderSourceHooks] byte gate failed at '{}' (Address Library ID {} + 0x{:X}); expected canonical TESNPC load, found [{}]",
                                  site.name, site.functionID.id(), site.offset, BytesAt(location.address(), site.expectedInstruction.size()));
                    valid = false;
                }
            }
            return valid;
        }

        bool PreflightTextureIdentitySites(std::array<std::uintptr_t, kTextureIdentitySites.size()> &a_addresses)
        {
            bool valid = true;
            for (std::size_t i = 0; i < kTextureIdentitySites.size(); ++i)
            {
                const auto &site = kTextureIdentitySites[i];
                const REL::Relocation<std::uintptr_t> location{site.functionID, site.offset};
                a_addresses[i] = location.address();
                if (std::memcmp(reinterpret_cast<const void *>(location.address()), site.expectedInstructions.data(), site.instructionSize) != 0)
                {
                    REX::CRITICAL("[RenderSourceHooks] byte gate failed at '{}' (Address Library ID {} + 0x{:X}); expected face-texture identity sequence, found [{}]",
                                  site.name, site.functionID.id(), site.offset, BytesAt(location.address(), site.instructionSize));
                    valid = false;
                }
            }
            return valid;
        }

        bool PreflightGeometryLookup(std::uintptr_t &a_address)
        {
            const REL::Relocation<std::uintptr_t> location{kGeometryAttachmentID, kGeometryLookupOffset};
            a_address = location.address();
            if (std::memcmp(reinterpret_cast<const void *>(a_address), kGeometryLookupInstruction.data(), kGeometryLookupInstruction.size()) == 0)
            {
                return true;
            }

            REX::CRITICAL("[RenderSourceHooks] byte gate failed at geometry attachment lookup (Address Library ID {} + 0x{:X}); expected FaceDB resource virtual call, found [{}]",
                          kGeometryAttachmentID.id(), kGeometryLookupOffset, BytesAt(a_address, kGeometryLookupInstruction.size()));
            return false;
        }

        constexpr bool IsPowerOfTwo(const std::size_t a_value) noexcept
        {
            return a_value != 0 && (a_value & (a_value - 1)) == 0;
        }

        void ValidateReadView(const RenderSourceRegistryReadView &a_view)
        {
            if (!a_view.runtimeOperational || !a_view.slots || !IsPowerOfTwo(a_view.capacity) || !IsPowerOfTwo(a_view.slotSize) || a_view.slotSize < sizeof(RE::TESNPC *))
            {
                throw std::runtime_error("render-source registry has an invalid generated-reader layout");
            }
            if (a_view.capacity > std::numeric_limits<std::size_t>::max() / a_view.slotSize)
            {
                throw std::runtime_error("render-source registry byte span overflowed");
            }

            const auto byteSpan = a_view.capacity * a_view.slotSize;
            if (!IsPowerOfTwo(byteSpan) || byteSpan - 1 > static_cast<std::size_t>(std::numeric_limits<std::int32_t>::max()))
            {
                throw std::runtime_error("render-source registry byte span cannot be encoded by the generated reader");
            }
            //i cry but :shrug:
            if (a_view.formIDOffset > a_view.slotSize - sizeof(std::uint64_t) || a_view.sourceOffset > a_view.slotSize - sizeof(RE::TESNPC *) ||
                a_view.activeOffset > a_view.slotSize - sizeof(std::uint64_t) || a_view.formIDOffset > static_cast<std::size_t>(std::numeric_limits<std::int32_t>::max()) ||
                a_view.sourceOffset > static_cast<std::size_t>(std::numeric_limits<std::int32_t>::max()) || a_view.activeOffset > static_cast<std::size_t>(std::numeric_limits<std::int32_t>::max()) ||
                a_view.formIDOffset % alignof(std::uint64_t) != 0 || a_view.sourceOffset % alignof(RE::TESNPC *) != 0 ||
                a_view.activeOffset % alignof(std::uint64_t) != 0 || reinterpret_cast<std::uintptr_t>(a_view.slots) % alignof(RE::TESNPC *) != 0)
            {
                throw std::runtime_error("render-source registry field offsets are invalid");
            }
            if (a_view.foldShift >= 64 || a_view.hashMultiplier == 0)
            {
                throw std::runtime_error("render-source registry hash parameters are invalid");
            }
        }

        std::array<Gpr, kScratchRegisterCount> SelectScratchRegisters(const Gpr a_result)
        {
            std::array<Gpr, kScratchRegisterCount> selected{};
            std::size_t count = 0;
            for (const auto candidate : kScratchCandidates)
            {
                if (candidate != a_result)
                {
                    selected[count++] = candidate;
                    if (count == selected.size())
                    {
                        return selected;
                    }
                }
            }
            throw std::runtime_error("could not select render-source thunk scratch registers");
        }

        class BaseReadThunk final : public Xbyak::CodeGenerator
        {
        public:
            BaseReadThunk(
                const InlineBaseReadSite &a_site,
                const RenderSourceRegistryReadView &a_view,
                HookTelemetry &a_telemetry) : Xbyak::CodeGenerator(kMaxThunkSize)
            {
                if (a_site.actorRegister == Gpr::kRsp || a_site.resultRegister == Gpr::kRsp)
                {
                    throw std::runtime_error("render-source thunk cannot use RSP as an actor or result register");
                }

                const Xbyak::Reg64 actor{static_cast<int>(a_site.actorRegister)};
                const Xbyak::Reg64 result{static_cast<int>(a_site.resultRegister)};
                const auto scratchRegisters = SelectScratchRegisters(a_site.resultRegister);
                const Xbyak::Reg64 byteOffset{static_cast<int>(scratchRegisters[0])};
                const Xbyak::Reg64 table{static_cast<int>(scratchRegisters[1])};
                const Xbyak::Reg64 key{static_cast<int>(scratchRegisters[2])};
                const Xbyak::Reg32 key32{static_cast<int>(scratchRegisters[2])};
                const Xbyak::Reg64 remaining{static_cast<int>(scratchRegisters[3])};
                const auto byteSpan = a_view.capacity * a_view.slotSize;
                const auto slotShift = static_cast<std::uint8_t>(std::countr_zero(a_view.slotSize));

                Xbyak::Label probe;
                Xbyak::Label found;
                Xbyak::Label done;

                pushfq();
                for (const auto scratch : scratchRegisters)
                {
                    push(Xbyak::Reg64{static_cast<int>(scratch)});
                }

                // Preserve the exact contract of the replaced MOV: only the result register changes, and a null canonical pointer remains null.
                mov(result, ptr[actor + 0x98]);
                test(result, result);
                jz(done, T_NEAR);

                // Terminal runtime shutdown must bypass sources already published in the immutable registry.
                mov(table, reinterpret_cast<std::uintptr_t>(a_view.runtimeOperational));
                cmp(byte[table], 0);
                jz(done, T_NEAR);

                // Key by the runtime FormID rather than the TESNPC pointer. Save loading may replace a dynamic leveled TESNPC object while preserving its FormID, so this binding survives that rebuild.
                mov(key32, dword[result + 0x28]);
                test(key32, key32);
                jz(done, T_NEAR);

                // Mirror StartIndex without crossing the C++ ABI. FormID keys are append-only, and aligned x86-64 loads provide the acquire ordering required after observing a published key.
                mov(byteOffset, key);
                mov(table, a_view.hashMultiplier);
                imul(byteOffset, table);
                mov(table, byteOffset);
                shr(table, a_view.foldShift);
                xor_(byteOffset, table);
                and_(byteOffset, static_cast<std::uint32_t>(a_view.capacity - 1));
                shl(byteOffset, slotShift);

                mov(table, reinterpret_cast<std::uintptr_t>(a_view.slots));
                mov(remaining, static_cast<std::uint64_t>(a_view.capacity));

                L(probe);
                cmp(qword[table + byteOffset + static_cast<int>(a_view.formIDOffset)], key);
                je(found, T_NEAR);
                cmp(qword[table + byteOffset + static_cast<int>(a_view.formIDOffset)], 0);
                jz(done, T_NEAR);

                add(byteOffset, static_cast<std::uint32_t>(a_view.slotSize));
                and_(byteOffset, static_cast<std::uint32_t>(byteSpan - 1));
                dec(remaining);
                jnz(probe);
                jmp(done, T_NEAR);

                L(found);
                mov(remaining, ptr[table + byteOffset + static_cast<int>(a_view.activeOffset)]);
                test(remaining, remaining);
                jz(done, T_NEAR);
                mov(remaining, ptr[table + byteOffset + static_cast<int>(a_view.sourceOffset)]);
                test(remaining, remaining);
                jz(done, T_NEAR);

                // Record successful substitutions without crossing the C++ ABI
                // from an engine appearance worker.
                mov(table, reinterpret_cast<std::uintptr_t>(std::addressof(a_telemetry.lastBaseFormID)));
                mov(dword[table], key32);
                mov(table, reinterpret_cast<std::uintptr_t>(std::addressof(a_telemetry.hitCount)));
                lock();
                inc(qword[table]);
                mov(result, remaining);

                L(done);
                for (auto it = scratchRegisters.rbegin(); it != scratchRegisters.rend(); ++it)
                {
                    pop(Xbyak::Reg64{static_cast<int>(*it)});
                }
                popfq();
                ret();
                ready();

                if (getSize() > kMaxThunkSize)
                {
                    throw std::runtime_error("base-read thunk exceeded its generation limit");
                }
            }
        };

        class GeometryLookupTelemetryThunk final : public Xbyak::CodeGenerator
        {
        public:
            explicit GeometryLookupTelemetryThunk(GeometryLookupTelemetry &a_telemetry) : Xbyak::CodeGenerator(kMaxThunkSize)
            {
                Xbyak::Label recordBase;
                Xbyak::Label done;

                // Preserve the original indirect-call contract. RAX contains
                // the resource vtable, RCX the resource, RDX the head-part
                // model key, and R12 the actor whose geometry is being built.
                push(rcx);
                push(rdx);
                sub(rsp, 0x28);
                call(ptr[rax + 0x1F0]);
                add(rsp, 0x28);
                pop(r11);
                pop(r10);

                mov(rcx, reinterpret_cast<std::uintptr_t>(std::addressof(a_telemetry.lastResourceAddress)));
                mov(qword[rcx], r10);
                mov(edx, dword[r11 - 0x48]);
                mov(rcx, reinterpret_cast<std::uintptr_t>(std::addressof(a_telemetry.lastHeadPartFormID)));
                mov(dword[rcx], edx);
                mov(rcx, reinterpret_cast<std::uintptr_t>(std::addressof(a_telemetry.lastEntryAddress)));
                mov(qword[rcx], rax);

                mov(r10, reinterpret_cast<std::uintptr_t>(std::addressof(a_telemetry.lookupCount)));
                lock();
                inc(qword[r10]);
                test(rax, rax);
                jnz(recordBase);
                mov(r10, reinterpret_cast<std::uintptr_t>(std::addressof(a_telemetry.missCount)));
                lock();
                inc(qword[r10]);

                L(recordBase);
                mov(r10, ptr[r12 + 0x98]);
                test(r10, r10);
                jz(done);
                mov(r10d, dword[r10 + 0x28]);
                mov(r11, reinterpret_cast<std::uintptr_t>(std::addressof(a_telemetry.lastBaseFormID)));
                mov(dword[r11], r10d);

                L(done);
                ret();
                ready();

                if (getSize() > kMaxThunkSize)
                {
                    throw std::runtime_error("geometry-lookup telemetry thunk exceeded its generation limit");
                }
            }
        };

        class TextureFormIDLookupThunk final : public Xbyak::CodeGenerator
        {
        public:
            explicit TextureFormIDLookupThunk(const std::uintptr_t a_tailTarget) : Xbyak::CodeGenerator(kMaxThunkSize)
            {
                // Preserve the manager pointer already in RCX, recover the runtime FormID for RSI, then tail-call the specialized FaceDB lookup from the stolen block.
                push(rcx);
                sub(rsp, 0x20);
                mov(rcx, rsi);
                // FaceDB selection lookup, not final texture naming.
                // Keep tied to the runtime base; substituting the configured FormID selects an unrelated entry from this specialized table.
                mov(rax, reinterpret_cast<std::uintptr_t>(&ResolveRuntimeFormIDForRenderSource));
                call(rax);
                mov(edx, eax);
                add(rsp, 0x20);
                pop(rcx);
                mov(rax, a_tailTarget);
                jmp(rax);
                ready();

                if (getSize() > kMaxThunkSize)
                {
                    throw std::runtime_error("face-texture FormID lookup thunk exceeded its generation limit");
                }
            }
        };

        class TextureOwnerFileThunk final : public Xbyak::CodeGenerator
        {
        public:
            explicit TextureOwnerFileThunk(const std::uintptr_t a_tailTarget) : Xbyak::CodeGenerator(kMaxThunkSize)
            {
                // The stolen sequence sets RCX to the NPC and calls GetFile.
                // Substitute the stable configured texture identity only for that call.
                sub(rsp, 0x28);
                mov(rcx, rsi);
                mov(rax, reinterpret_cast<std::uintptr_t>(&ResolveFaceTextureIdentityForRenderSource));
                call(rax);
                add(rsp, 0x28);
                mov(rcx, rax);
                mov(rax, a_tailTarget);
                jmp(rax);
                ready();

                if (getSize() > kMaxThunkSize)
                {
                    throw std::runtime_error("face-texture owner-file thunk exceeded its generation limit");
                }
            }
        };

        class TextureMaskedFormIDThunk final : public Xbyak::CodeGenerator
        {
        public:
            TextureMaskedFormIDThunk() : Xbyak::CodeGenerator(kMaxThunkSize)
            {
                // Both owner-file branches converge here with their plugin-tier FormID mask in EAX.
                // Preserve that mask and every volatile register the stolen block left untouched across the C++ reverse lookup, then reproduce `EDX = FormID; EBX = EDX & EAX`.
                push(rax);
                push(rcx);
                push(r8);
                push(r9);
                push(r10);
                push(r11);
                sub(rsp, 0x28);
                mov(rcx, rsi);
                mov(rax, reinterpret_cast<std::uintptr_t>(&ResolveFaceTextureIdentityForRenderSource));
                call(rax);
                mov(edx, dword[rax + 0x28]);
                add(rsp, 0x28);
                pop(r11);
                pop(r10);
                pop(r9);
                pop(r8);
                pop(rcx);
                pop(rax);
                mov(ebx, edx);
                and_(ebx, eax);
                ret();
                ready();

                if (getSize() > kMaxThunkSize)
                {
                    throw std::runtime_error("face-texture masked-FormID thunk exceeded its generation limit");
                }
            }
        };

        class CompositeMaskedFormIDThunk final : public Xbyak::CodeGenerator
        {
        public:
            CompositeMaskedFormIDThunk() : Xbyak::CodeGenerator(kMaxThunkSize)
            {
                // The compositor has the FormID mask in EAX and the detached TESNPC in RSI.
                // Preserve every other volatile register, then reproduce the stolen `FormID & mask` result in EDI.
                push(rax);
                push(rcx);
                push(rdx);
                push(r8);
                push(r9);
                push(r10);
                push(r11);
                sub(rsp, 0x20);
                mov(rcx, rsi);
                mov(rax, reinterpret_cast<std::uintptr_t>(&ResolveFaceTextureIdentityForRenderSource));
                call(rax);
                mov(edi, dword[rax + 0x28]);
                add(rsp, 0x20);
                pop(r11);
                pop(r10);
                pop(r9);
                pop(r8);
                pop(rdx);
                pop(rcx);
                pop(rax);
                and_(edi, eax);
                ret();
                ready();

                if (getSize() > kMaxThunkSize)
                {
                    throw std::runtime_error("face-texture compositor FormID thunk exceeded its generation limit");
                }
            }
        };

        class CompositeOwnerIndexThunk final : public Xbyak::CodeGenerator
        {
        public:
            CompositeOwnerIndexThunk() : Xbyak::CodeGenerator(kMaxThunkSize)
            {
                // The compositor uses the TESNPC load-order index to select the owner filename embedded in its output cache key.
                // A detached source and its dynamic runtime base have no registered owner.
                // Use the configured NPC's real plugin owner for a stable generated-texture key.
                Xbyak::Label ownerReady;

                pushfq();
                push(rax);
                push(rcx);
                push(r8);
                push(r9);
                push(r11);
                sub(rsp, 0x28);
                mov(rcx, rsi);
                mov(rax, reinterpret_cast<std::uintptr_t>(&ResolveFaceTextureIdentityForRenderSource));
                call(rax);
                movzx(edx, word[rax + 0x30]);
                mov(r10d, 0xFFFF);
                cmp(dx, r10w);
                jne(ownerReady);
                xor_(edx, edx);
                L(ownerReady);
                add(rsp, 0x28);
                pop(r11);
                pop(r9);
                pop(r8);
                pop(rcx);
                pop(rax);
                popfq();
                ret();
                ready();

                if (getSize() > kMaxThunkSize)
                {
                    throw std::runtime_error("face-texture compositor owner-index thunk exceeded its generation limit");
                }
            }
        };

        GeneratedThunk CopyThunk(REL::Trampoline &a_trampoline, const Xbyak::CodeGenerator &a_thunk)
        {
            const auto size = a_thunk.getSize();
            auto *memory = a_trampoline.allocate(size);
            std::memcpy(memory, a_thunk.getCode(), size);
            return GeneratedThunk{
                .address = reinterpret_cast<std::uintptr_t>(memory),
                .size = size};
        }

        GeneratedThunk GenerateThunk(
            REL::Trampoline &a_trampoline,
            const InlineBaseReadSite &a_site,
            const RenderSourceRegistryReadView &a_view,
            HookTelemetry &a_telemetry)
        {
            BaseReadThunk thunk{a_site, a_view, a_telemetry};
            return CopyThunk(a_trampoline, thunk);
        }

        GeneratedThunk GenerateTextureIdentityThunk(REL::Trampoline &a_trampoline, const InlineTextureIdentitySite &a_site)
        {
            switch (a_site.kind)
            {
            case TextureIdentityKind::kFormIDThenLookup:
            {
                const REL::Relocation<std::uintptr_t> target{a_site.tailTarget};
                TextureFormIDLookupThunk thunk{target.address()};
                return CopyThunk(a_trampoline, thunk);
            }
            case TextureIdentityKind::kOwnerFileLookup:
            {
                const REL::Relocation<std::uintptr_t> target{a_site.tailTarget};
                TextureOwnerFileThunk thunk{target.address()};
                return CopyThunk(a_trampoline, thunk);
            }
            case TextureIdentityKind::kMaskedFormID:
            {
                TextureMaskedFormIDThunk thunk;
                return CopyThunk(a_trampoline, thunk);
            }
            case TextureIdentityKind::kCompositeOwnerIndex:
            {
                CompositeOwnerIndexThunk thunk;
                return CopyThunk(a_trampoline, thunk);
            }
            case TextureIdentityKind::kCompositeMaskedFormID:
            {
                CompositeMaskedFormIDThunk thunk;
                return CopyThunk(a_trampoline, thunk);
            }
            }
            throw std::runtime_error("unknown face-texture identity thunk kind");
        }

        GeneratedThunk GenerateGeometryLookupTelemetryThunk(REL::Trampoline &a_trampoline, GeometryLookupTelemetry &a_telemetry)
        {
            GeometryLookupTelemetryThunk thunk{a_telemetry};
            return CopyThunk(a_trampoline, thunk);
        }

        bool PrepareCallPatch(PreparedPatch &a_patch, const std::uintptr_t a_address, const GeneratedThunk &a_thunk, const std::size_t a_size, const std::string_view a_name)
        {
            if (a_size < 5 || a_size > a_patch.bytes.size())
            {
                throw std::runtime_error("invalid inline patch size");
            }

            const auto displacement64 = static_cast<std::int64_t>(a_thunk.address) - static_cast<std::int64_t>(a_address + 5);
            if (displacement64 < std::numeric_limits<std::int32_t>::min() || displacement64 > std::numeric_limits<std::int32_t>::max())
            {
                REX::CRITICAL("[RenderSourceHooks] trampoline for '{}' is outside rel32 range; no engine code was patched", a_name);
                return false;
            }

            a_patch.address = a_address;
            a_patch.size = a_size;
            a_patch.bytes.fill(REL::NOP);
            a_patch.bytes[0] = 0xE8;
            const auto displacement = static_cast<std::int32_t>(displacement64);
            std::memcpy(a_patch.bytes.data() + 1, &displacement, sizeof(displacement));
            return true;
        }

    }

    bool InstallRenderSourceHooks() noexcept
    {
        if (g_installed.load(std::memory_order_acquire))
        {
            return true;
        }

        try
        {
            std::array<std::uintptr_t, kBaseReadSites.size()> addresses{};
            std::array<std::uintptr_t, kTextureIdentitySites.size()> textureIdentityAddresses{};
            std::uintptr_t geometryLookupAddress = 0;
            if (!PreflightFaceTextureCompositorContract() || !PreflightSites(addresses) || !PreflightTextureIdentitySites(textureIdentityAddresses) ||
                !PreflightGeometryLookup(geometryLookupAddress))
            {
                REX::CRITICAL("[RenderSourceHooks] one or more appearance or compositor sites did not match; no engine code was patched");
                return false;
            }

            const auto readView = GetRenderSourceRegistryReadView();
            ValidateReadView(readView);

            auto &trampoline = REL::GetTrampoline();
            std::array<PreparedPatch, kBaseReadSites.size()> patches{};
            std::array<PreparedPatch, kTextureIdentitySites.size()> textureIdentityPatches{};
            PreparedPatch geometryLookupPatch{};
            std::size_t generatedBytes = 0;
            for (std::size_t i = 0; i < kBaseReadSites.size(); ++i)
            {
                const auto thunk = GenerateThunk(trampoline, kBaseReadSites[i], readView, g_hookTelemetry[i]);
                generatedBytes += thunk.size;
                if (!PrepareCallPatch(patches[i], addresses[i], thunk, kBaseReadSites[i].expectedInstruction.size(), kBaseReadSites[i].name))
                {
                    return false;
                }
            }
            for (std::size_t i = 0; i < kTextureIdentitySites.size(); ++i)
            {
                const auto thunk = GenerateTextureIdentityThunk(trampoline, kTextureIdentitySites[i]);
                generatedBytes += thunk.size;
                if (!PrepareCallPatch(textureIdentityPatches[i], textureIdentityAddresses[i], thunk, kTextureIdentitySites[i].instructionSize, kTextureIdentitySites[i].name))
                {
                    return false;
                }
            }
            const auto geometryLookupThunk = GenerateGeometryLookupTelemetryThunk(trampoline, g_geometryLookupTelemetry);
            generatedBytes += geometryLookupThunk.size;
            if (!PrepareCallPatch(geometryLookupPatch, geometryLookupAddress, geometryLookupThunk, kGeometryLookupInstruction.size(), "FaceDB geometry lookup telemetry"))
            {
                return false;
            }

            for (const auto &patch : patches)
            {
                REL::Relocation<std::uintptr_t>{patch.address}.write(patch.bytes.data(), patch.size);
            }
            for (const auto &patch : textureIdentityPatches)
            {
                REL::Relocation<std::uintptr_t>{patch.address}.write(patch.bytes.data(), patch.size);
            }
            REL::Relocation<std::uintptr_t>{geometryLookupPatch.address}.write(geometryLookupPatch.bytes.data(), geometryLookupPatch.size);
            g_installed.store(true, std::memory_order_release);
            REX::INFO("[RenderSourceHooks] installed {} NPC read redirects, {} face-texture identity redirects, and geometry lookup telemetry ({} generated bytes)",
                      kBaseReadSites.size(), kTextureIdentitySites.size(), generatedBytes);
            return true;
        }
        catch (const std::exception &error)
        {
            REX::CRITICAL("[RenderSourceHooks] installation threw before completion: {}", error.what());
        }
        catch (...)
        {
            REX::CRITICAL("[RenderSourceHooks] installation threw an unknown exception before completion");
        }
        return false;
    }

    void ReportRenderSourceHookTelemetry() noexcept
    {
        if (!g_installed.load(std::memory_order_acquire))
        {
            return;
        }

        for (std::size_t i = 0; i < g_hookTelemetry.size(); ++i)
        {
            auto &telemetry = g_hookTelemetry[i];
            const auto baseID = telemetry.lastBaseFormID.load(std::memory_order_relaxed);
            const auto hitCount = telemetry.hitCount.load(std::memory_order_relaxed);
            const auto previouslyReportedBaseID = telemetry.reportedBaseFormID.exchange(baseID, std::memory_order_relaxed);
            const auto previouslyReportedHitCount = telemetry.reportedHitCount.exchange(hitCount, std::memory_order_relaxed);
            if (baseID == 0 || (previouslyReportedBaseID == baseID && previouslyReportedHitCount == hitCount))
            {
                continue;
            }

            REX::DEBUG("[RenderSourceHooks] source consumed at '{}' for base=0x{:08X} totalHits={}",
                       kBaseReadSites[i].name, baseID, hitCount);
        }

        const auto lookupCount = g_geometryLookupTelemetry.lookupCount.load(std::memory_order_relaxed);
        if (lookupCount != 0 && g_geometryLookupTelemetry.reportedLookupCount.exchange(lookupCount, std::memory_order_relaxed) != lookupCount)
        {
            REX::DEBUG("[RenderSourceHooks] FaceDB hair-class geometry lookup for base=0x{:08X} headPart=0x{:08X} resource=0x{:016X} entry=0x{:016X} totalLookups={} totalMisses={}",
                       g_geometryLookupTelemetry.lastBaseFormID.load(std::memory_order_relaxed),
                       g_geometryLookupTelemetry.lastHeadPartFormID.load(std::memory_order_relaxed),
                       g_geometryLookupTelemetry.lastResourceAddress.load(std::memory_order_relaxed),
                       g_geometryLookupTelemetry.lastEntryAddress.load(std::memory_order_relaxed),
                       lookupCount,
                       g_geometryLookupTelemetry.missCount.load(std::memory_order_relaxed));
        }
    }
}
