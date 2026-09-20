// In-process IL2CPP dumper.
//
// Il2CppDumper is a file parser and has no live-process mode (verified: zero
// occurrences of OpenProcess / ReadProcessMemory / VirtualQuery in its source),
// so it cannot be pointed at a running game.  It also cannot read this target's
// `global-metadata.dat`, because that file is encrypted on disk (measured: the
// sanity word reads back as 0x1357FEDA instead of 0xFAB11BAF, the magic appears
// zero times, entropy 6.58 bit/byte -- a transformation, not strong crypto).
//
// Both problems have the same answer: ask the running process.  It has already
// decrypted the metadata in order to run, and it holds the fully-formed type
// system.  So this module does two independent things:
//
//   * FindMetadataBlob() recovers the *plaintext* metadata from memory, which
//     turns the encryption into a non-problem and hands Il2CppDumper the input
//     it expects.  This is the path to complete data.
//   * DumpTypeSystem() writes a dump.cs-style inventory straight out of the live
//     type system.  Its unique value is runtime-verified field offsets, which no
//     file parser can produce.
//
// Neither one needs the other, and either is useful alone.
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace cabbird::il2cpp {

// A plaintext IL2CPP global-metadata blob recovered from this process.
struct MetadataBlob {
    std::uintptr_t address{};   // where it was found, for the log
    std::size_t size{};         // total bytes, sized from the blob's own header
    std::int32_t version{};     // metadata version, e.g. 29 for Unity 2022.3
    // MEM_PRIVATE or MEM_MAPPED.  Stored as a plain integer rather than DWORD so
    // this header does not have to drag in <Windows.h> for two constants; the
    // implementation maps it to a name for the log.
    std::uint32_t region_type{};
    std::vector<std::uint8_t> bytes;
};

// How well a candidate header looks like a real metadata header.
//
// The magic word alone proves almost nothing: scanning ~1.4 GB puts the odds of a
// 4-byte coincidence at roughly one in three.  So acceptance cannot rest on the
// version field either -- a coincidence has a random version, and a real header
// whose version field we do not recognise is indistinguishable from it by that
// field alone.  What a coincidence cannot fake is the structure: a real header is
// dozens of (offset, size) pairs describing tables that lie inside the blob.
struct MetadataHeaderScore {
    std::size_t sane_pairs{};    // plausible pairs before the first implausible one
    std::size_t derived_size{};  // max(offset+size) over those pairs
};

// Scores a candidate.  `sane_pairs` is the discriminator; see kMinSanePairs in
// the implementation for the threshold and why it is safe.
MetadataHeaderScore ScoreMetadataHeader(const std::uint8_t* header, std::size_t available);

// Sizes a metadata blob from its own header.
//
// IL2CPP records no total length.  Every field after the two-word preamble is an
// (offset, size) pair relative to the blob start, so the largest offset+size is
// the end of the last table -- i.e. the blob size.  The parse has to stop at the
// first implausible pair rather than reading a fixed number of them: past the end
// of the real header sits actual table data, and interpreting that as pairs
// yields absurd offsets that would blow the size up to gigabytes.
//
// Returns 0 when the header does not look like a metadata header.
std::size_t SizeMetadataFromHeader(const std::uint8_t* header, std::size_t available);

// Scans this process's committed memory for the plaintext metadata header and
// copies the whole blob out.
//
// Three passes, in likelihood order:
//   1. MEM_PRIVATE -- IL2CPP reads the file into a malloc'd buffer on most
//      platforms, so this is where the blob usually is.
//   2. MEM_MAPPED  -- but the runtime may instead map the file and decrypt it in
//      place, in which case a private-only scan misses it completely and reports
//      the same "not found" as a genuinely absent blob.  The first version of
//      this function had exactly that hole.
//   3. MEM_IMAGE   -- a metadata blob is not a PE image, so this is the least likely
//      pass of the three; it is here because a "not found" from a narrower scan
//      cannot be told apart from "it was in a region type we refused to look at".
//
// MEASURED on this target: the blob lands in a MEM_MAPPED region (type 0x40000) at
// 0x000000000C4F0150, so pass 2 is the one that finds it here.
//
// Every candidate that matched the magic is reported through `diagnostics`, with
// its version, derived size and structural score -- accepted or not.  A run that
// fails should say what it saw, not just that it found nothing.
//
// Bounded by design: 8 MiB scan windows (so a multi-gigabyte heap is never
// copied wholesale) and a hard cap on the blob size.
std::optional<MetadataBlob> FindMetadataBlob(std::string& error, std::string* diagnostics = nullptr);

// The same recovery, cached: this is what a service exposes.
//
// WHY A CACHE RATHER THAN A CALLER-SIDE COPY: the blob is 50 MB and the scan costs
// ~6.6 s (measured).  A service a plugin may poll -- or ask twice, once per domain --
// cannot pay that per call, and it must not hold the answer somewhere that dies with
// the caller.  So the process owns one blob and this is how it is asked for.
//
// The first successful call latches; later calls return the SAME bytes even if the
// runtime has since moved or freed them, which is the point of copying at all.
// `error` is left alone on a cache hit.  `elapsed_ms` (optional) receives the scan
// time of the call that produced the blob, and 0 for a cached hit.
[[nodiscard]] const MetadataBlob* CachedMetadataBlob(std::string& error,
                                                     std::uint64_t* elapsed_ms = nullptr);

// Forgets the cached blob so the next CachedMetadataBlob() rescans.  Exists for callers that
// plant and remove blobs in their own address space.
void ResetMetadataCache();

// Writes a recovered blob to `path`.
//
// The caller supplies the path because WHERE a 50 MB artifact lands is a policy
// question -- the host writes beside its dump, the offline test writes to a temp
// directory -- and this library has no business guessing.
bool WriteMetadataBlob(const MetadataBlob& blob, const std::wstring& path, std::string& error);


struct DumpOptions {
    // 0 means "no limit" for each of these.
    std::size_t max_classes_per_image{};
    std::size_t max_fields_per_class{};
    std::size_t max_methods_per_class{};

    bool include_fields{true};

    // Method enumeration is OFF by default, and that is a deliberate risk call
    // rather than an oversight.
    //
    // il2cpp_class_get_fields reads offsets that the runtime has already computed.
    // il2cpp_class_get_methods instead forces lazy metadata initialisation for
    // every method in the image and allocates managed memory while doing it --
    // across ~22,000 classes that is by far the heaviest thing this project does
    // inside the game, and the failure mode is a crash that costs a launch.
    bool include_methods{false};

    // Restricts method dumping to images whose name contains one of these
    // substrings.  Empty means every image.
    //
    // This is what makes `include_methods` usable at all.  Calling
    // il2cpp_class_get_methods forces lazy initialisation and allocates managed
    // memory, and doing that across every class in the process -- including
    // thousands of runtime-internal ones we do not care about -- is exactly the
    // heavy, crash-prone operation the note above warns about.  The game's own
    // assemblies are a small fraction of the 22,192 classes, so a filter such as
    // {"Azur"} bounds the work to the part that matters.
    //
    // Substring rather than exact match on purpose: the target splits its code
    // across AzurFrameworkRuntime / AzurShell / AzurEngine / AzurCore /
    // AzurPrecompile, and a per-image list would need updating every patch.
    //
    // ONE ENTRY PER NEEDLE.  The "-or-space separated" convention a user types
    // (`Azur,Assembly-CSharp` or `Azur Proxima`) is split at the CONFIGURATION EDGE, by
    // whoever read the string; this vector is the RESULT of that split, not the raw text.
    //
    // That split is load-bearing, and its absence was a real defect rather than a style
    // question: the caller used to push the whole string as ONE entry, so a match required
    // an image name containing all 32 characters of "Azur,Assembly-CSharp,Proxima,Lens" --
    // a test no image can pass.  A single-word default ("Azur") hid it, because then the
    // whole string WAS a valid needle.  The failure mode is silent by construction: the
    // walk finds nothing, `stats.methods` is 0, and the dump still looks complete.
    std::vector<std::string> method_image_filter;

    // GameAssembly.dll's load range, used to turn a method pointer into an RVA
    // and, just as importantly, to VALIDATE it.
    //
    // There is no exported getter for `MethodInfo::methodPointer`, so it has to
    // be read as a raw pointer at offset 0 of MethodInfo.  That offset comes from
    // Unity's own header, but from a DIFFERENT Unity version than this target
    // runs, so it cannot simply be trusted.  The range check turns the assumption
    // into a measurement: a pointer that lands inside the module is real code, and
    // one that does not is reported as unknown rather than printed as a confident
    // wrong address.  DumpStats::method_pointers_inside / _outside report the
    // score, so a wrong offset shows up as a number instead of as silent garbage.
    //
    // WHEN THIS IS LEFT AT ZERO THE OUTPUT IS STILL VALID, BUT THE RVAs ARE NOT:
    // with no base the dumper prints the raw pointer under the name "RVA", so the
    // same method gets a different number every launch (ASLR) and every consumer
    // silently loses the ability to file the dump against the on-disk image.  Left at
    // zero, the output carries three columns -- `// RVA:`, `Offset:` and `VA:` -- holding
    // one raw VA.  The host fills these
    // from the loaded module; a caller that supplies its own blob leaves them zero on purpose.
    std::uintptr_t module_base{};
    std::uintptr_t module_size{};

    // Where the recovered plaintext metadata blob should be written, if the caller
    // asks for recovery as part of the dump (see DumpStats::metadata_size).
    // Empty means "recover into memory only", which is what a caller that reads the
    // bytes back through a service wants.
    std::wstring metadata_path;


    // OFF by default, and that default is load-bearing.
    //
    // `il2cpp_class_get_fields` is a pure metadata read.  These two are not:
    // internally they run Class::SetupProperties / Class::SetupInterfaces, which
    // WRITE to the class structure and can allocate.  Called from a thread that is
    // not attached to the IL2CPP runtime, while the game is initialising the same
    // class, they can dereference a half-built pointer.
    //
    // That is not a theory.  An in-game run with both enabled produced
    // 0xC0000005 in GameAssembly.dll + 0x13E15C2 reading from address 0x135 --
    // i.e. a null base plus the offset of Il2CppClass::instance_size/actualSize --
    // on the probe thread, during the dump, while the fields-only path had
    // completed cleanly on the same game.  They stay opt-in until they have been
    // re-tested with DumpOptions::attach_thread set.
    bool include_properties{false};
    bool include_interfaces{false};

    // Attach the calling thread to the IL2CPP domain for the duration of the dump
    // (il2cpp_thread_attach / il2cpp_thread_detach).
    //
    // Every il2cpp API that lazily initialises a class is documented as requiring
    // an attached thread, because that is the only state in which the runtime's
    // per-thread bookkeeping exists.  Our probe thread is one we created, so it
    // starts unattached.  Set this before enabling any initialising walk.
    bool attach_thread{true};

    // Progress reporting, called from the walker's OWN thread after each image and
    // about every `progress_class_interval` classes within an image.
    //
    // WHY A CALLBACK RATHER THAN A STATS READER: the walk holds no lock, so a reader on
    // another thread has nothing consistent to read mid-walk.  Handing the numbers out
    // from the walk itself is the only way to publish progress without either locking the
    // hot loop or inventing a second copy of the counters that can drift.
    //
    // The `user` pointer is passed through untouched.  A callback that throws is the
    // caller's problem, not the walk's -- this header promises nothing about exceptions
    // because every existing caller passes a noexcept lambda.
    void (*progress)(void* user, std::size_t images, std::size_t classes,
                     std::size_t fields) noexcept{nullptr};
    void* progress_user{nullptr};
    // Throttles, whichever fires first.
    //
    // The class count is the WRONG primary trigger on its own, and a real-machine run
    // proved it: this was `progress_class_interval` alone (1024), and with `include_methods`
    // an image can hold few classes but enormous method counts, so several seconds passed
    // with no report at all.  The window then showed its stall warning
    // ("clock is advancing but no class has been read") while the walk was in fact making
    // progress -- a false alarm produced by the reporting schedule rather than by the work.
    //
    // Elapsed time is the trigger that matches what a reader wants ("tell me it is alive"),
    // so it is primary and the class count is the secondary bound for a machine where the
    // clock has low resolution.
    std::size_t progress_class_interval{64};
    std::uint64_t progress_interval_ms{500};

    // Recover the plaintext metadata blob as part of this dump.
    //
    // WHY IT IS WELDED TO THE DUMP RATHER THAN LEFT TO THE CALLER: the two answers a
    // reader of a dump.cs needs are "what does the live type system say" (this walk)
    // and "what does the whole image say" (Il2CppDumper, which needs the decrypted
    // metadata as input and cannot read the encrypted file on disk).  A run that
    // produced the first without the second shipped for weeks: 45,578 methods, none
    // of them from Assembly-CSharp.dll, because the metadata was never handed to the
    // parser that could see the whole image.  One request now produces both.
    //
    // Cost, measured: the scan is ~6.6 s over 2,356 MiB / 2,044 regions on this
    // target -- the same order as the method walk, so a caller that wants the walk
    // wants this too.  It is still a flag rather than the default so that a caller
    // that only wants field offsets does not pay for it.
    bool recover_metadata{false};
};

struct DumpStats {
    std::size_t images{};
    std::size_t classes{};
    std::size_t fields{};
    std::size_t methods{};
    std::size_t properties{};
    std::size_t truncated_classes{};
    // Method pointers that did / did not land inside the module range.  See
    // DumpOptions::module_base for why this is measured rather than assumed.
    std::size_t method_pointers_inside{};
    std::size_t method_pointers_outside{};
    // Faults swallowed by the SEH guards.  A non-zero value means the runtime was
    // not in a state our assumptions describe -- it is the signal that the dump is
    // incomplete, and printing it is the whole point of catching instead of dying.
    std::size_t contained_faults{};
    // Whether the thread was actually attached to the IL2CPP domain.
    bool thread_attached{};
    // Metadata recovery, when DumpOptions::recover_metadata asked for it.
    //
    // A dump that silently omits its own inputs is the failure this pair exists to
    // make visible: `metadata_size == 0` is a stated fact ("recovery was not asked
    // for, or it found nothing"), not a zero that could mean four different things.
    std::size_t metadata_size{};
    std::int32_t metadata_version{};
    std::uintptr_t metadata_address{};
    bool metadata_written{};       // the blob reached DumpOptions::metadata_path
    std::uint64_t metadata_scan_ms{};
};

// Appends a dump.cs-style listing of the live type system to `out`.
//
// Requires the calling thread to already be attached and registered with the
// runtime (see ThreadScope and the registration gate in the probe), because
// every call below touches managed structures.
DumpStats DumpTypeSystem(std::string& out, const DumpOptions& options = {});

}  // namespace cabbird::il2cpp
