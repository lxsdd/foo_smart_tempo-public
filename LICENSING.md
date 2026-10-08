# Third-party licenses and public binary release decision

Scope: `lxsdd/foo_smart_tempo-public` source tree as of 2026-10-08. This is
a conservative redistribution decision, not an assertion that all underlying
rights issues have been adjudicated or a substitute for legal counsel.

## Verified inputs

| Component | Location / provenance | License | Consequence |
| --- | --- | --- | --- |
| Audacity music-information-retrieval (Matthieu Hodgkinson) | `src/foo_smart_tempo/hodgkinson_full_mir.cpp`, `.h`: explicitly describe an algorithmic port of Audacity 3.7.7 `lib-music-information-retrieval` | `GPL-2.0-or-later` according to these source files and Audacity upstream | Treat this implementation as GPL-covered unless a documented independent-code provenance/other authorization establishes otherwise. A license notice cannot simply be deleted. |
| Windows Template Library (WTL) 10.1.0 | Downloaded in `.github/workflows/source.yml` from the public NuGet package, used by `bpm_preferences_page.h` and `bpm_result_dialog.h` and by SDK UI helpers | `MS-PL` | MS-PL is listed by the Free Software Foundation as **GNU GPL-incompatible**. WTL is not presumed to qualify as an operating-system system library merely because it targets Windows. |
| foobar2000 2.x SDK, including pfc/libPPUI | CI pins public SDK commit `9e4823ee802dfb704de1ce42401b48b407a54180` | SDK redistribution terms at `sdk/sdk-license.txt`, plus licenses inside pfc/libPPUI | Source redistribution and attribution must follow the appropriate SDK/library conditions. The proprietary foobar2000 host raises a **separate** GPL plug-in compatibility question if GPL-covered code is linked into its in-process component. |
| KissFFT | Vendored under `src/kiss_fft` | 3-clause BSD-like notice at `src/kiss_fft/COPYING` / `licenses/kissfft_license.txt` | Permissive; retain the binary-distribution attribution and disclaimer. |
| Locally written code | Most other `src/foo_smart_tempo/` sources | Copyright of respective authors, **no blanket license asserted here** | Do not infer permission to relicense all project files because one dependency is GPL-covered. |

## Outcome for the current `.fb2k-component`

**NOT CLEARED FOR PUBLIC BINARY REDISTRIBUTION.**

This currently links the Audacity-derived MIR implementation into a WTL-based
foobar2000 DLL. The FSF explicitly identifies MS-PL as GPL-incompatible;
this release also depends on the separate GPL/nonfree-host linkage assessment.
Merely publishing the source, changing the SPDX header, offering a GitHub
Actions artifact instead of a GitHub Release, or putting the same DLL in a
different ZIP does **not** fix the combined binary's redistribution status.

The public CI may compile, run synthetic tests, and temporarily package files
inside its ephemeral job. It must **not upload** component binaries, publish
releases, or advertise production-ready builds. Any prior Actions binary
artifacts must be purged independently of removing the upload step.

## Engineering decision: preserve UI, replace GPL-derived MIR path

For a distributable in-process component, prefer to replace **all copyrighted
GPL-derived MIR implementation** with a genuinely independently developed or
GPL-compatible-permissively licensed implementation, then confirm *every*
remaining binary dependency and foobar host/plugin interface condition. This
keeps the existing WTL preferences/result UI and most measured-candidate
policy code intact, instead of migrating a large GUI only to retain the
GPL/nonfree-host integration issue.

The replacement must **not** be a search-and-replace license header or a
translation of GPL code. Preserve the user's existing analysis behavior until
a new implementation is separately built, synthetically tested, and compared
on privately retained BPM holdouts (aggregate metrics only may leave the
owner's machine). Existing private releases and research remain untouched.

Acceptance before a published component:
1. Auditable provenance for every MIR implementation file and transitive
   dependency; no GPL-derived code linked into the distributable WTL DLL,
   unless actual rights-holder license exceptions are documented and validated.
2. License texts/attributions for all included sources and binary dependencies,
   including foobar2000 SDK/pfc/libPPUI, WTL and KissFFT.
3. Public CI verifies no forbidden license inputs are compiled into the
   candidate, and packages binary license notices correctly.
4. Functional synthetic CI plus private foobar runtime and real-audio accuracy
   comparison establish no unjustified BPM regression.
5. Legal review/explicit rights-holder authorization for any remaining
   ambiguity about host plug-in and dependency obligations, **before** binary
   publication.

This decision concerns redistribution only. Private use and normal source
development can continue without treating public binary release as cleared.

## Public primary sources

- Audacity file copyright/license:
  https://github.com/audacity/audacity/tree/master/libraries/lib-music-information-retrieval
- GNU FSF Microsoft Public License compatibility:
  https://www.gnu.org/licenses/license-list.html#ms-pl
- GNU FAQ on GPL plug-ins with nonfree programs:
  https://www.gnu.org/licenses/gpl-faq.html#GPLPluginsInNF
- Microsoft Public License legal text:
  https://opensource.org/license/MS-PL
- foobar2000 SDK license:
  https://github.com/reupen/foobar2000-sdk-unmodified/blob/master/sdk-license.txt
- Full GNU GPL version 2 text is included at `licenses/GPL-2.0-or-later.txt`
  (verbatim SPDX license-list-data copy; “or later” attaches to file notices,
  not a different body of the GPLv2 legal text).

No song names, BPM ground truth, owner filesystem paths, audio, or private
research reports are needed for this determination.
