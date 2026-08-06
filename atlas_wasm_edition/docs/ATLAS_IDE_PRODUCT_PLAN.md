# ATLAS IDE Industrial Product Plan

This is the product checklist for turning the ATLAS compiler into a complete
industrial IDE. Items marked "compiler-backed" have an implementation path in
the current C compiler; platform items belong in the future IDE/server layer.

## Language Features

| Feature | Status | Notes |
|---|---:|---|
| Documentation comments | Compiler-backed | `///`, `##`, and documentation block comments are preserved into generated ST comments. |
| Variable units | Compiler-backed | Declarations can use `temp IN celsius` or `pressure : REAL IN bar`; generated declarations include unit metadata. |
| Named constants | Compiler-backed | `MAX_TEMP = 95` or `CONST MAX_TEMP = 95` emits a `VAR CONSTANT` declaration. |
| Reusable recipes/templates | Compiler-backed scaffold | `USE boiler_startup_sequence` is parsed and emitted as an IDE recipe expansion hook. |
| Multi-language DSL | Compiler-backed seed | English plus Spanish, Hindi, and Arabic aliases for core logic keywords. |

## IDE Features

| Feature | Product Module |
|---|---|
| Live simulation | Simulation runtime service backed by the existing AST evaluator. |
| Drag-and-drop logic blocks | Visual block editor that serializes to ATLAS DSL. |
| Built-in version control | Project history service with commit metadata and branch policy. |
| Diff view | AST-aware diff for DSL and generated IEC output. |
| Auto-complete | Language server using lexer/parser symbol tables. |
| Real-time error highlighting | Incremental parser plus diagnostics feed. |
| Themes | UI shell with dark, light, and industrial grey themes. |

## Safety & Compliance

| Feature | Product Module |
|---|---|
| IEC 61508 PDF reports | Report renderer fed by `safety_analyze` and diagnostics output. |
| Compliance checklist | Deployment gate checklist using hardening/safety flags. |
| Mandatory sign-off | Workflow service: engineer approval, safety officer approval, deploy approval. |
| Audit trail | Append-only event log with timestamp, user, project, and hash chain. |
| E-stop coverage checker | Already compiler-backed through the safety analyzer. |

## Deployment

| Feature | Product Module |
|---|---|
| One-click vendor export | UI wrapper around Siemens, Rockwell, CODESYS, and PLCopen exporters. |
| Direct PLC upload | Vendor-specific secured connector; never bypass plant network policy. |
| Backup and restore | PLC connector plus signed artifact store. |
| Remote monitoring dashboard | Read-only telemetry connector with role-based permissions. |

## Business Features

| Feature | Product Module |
|---|---|
| Team collaboration | Project service with presence, comments, and review requests. |
| Project locking | Lock lease service with timeout and admin override. |
| Role-based access | RBAC policy engine: operator, engineer, safety officer, admin. |
| License management | Enterprise entitlement service with offline activation support. |

## Near-Term Build Order

1. Language server over the current lexer/parser/semantic pipeline.
2. IDE shell with editor, diagnostics panel, generated-output preview, and themes.
3. Project history, diff view, and audit log.
4. Safety PDF renderer and sign-off gates.
5. Vendor export UI and guarded deployment connectors.
