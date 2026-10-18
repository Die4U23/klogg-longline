# Project rules

- User-owned Git history: do not add Co-authored-by trailers or agent attribution. New commits must use the user's verified GitHub identity for both author and committer. Do not commit or push unless the coordinating user-facing task authorizes it.
- Do not merge or import upstream Git history. Import source snapshots only; retain upstream LICENSE/COPYING, NOTICE, copyright headers and provenance. Source attribution must not be removed or misrepresented.
- Follow klogg-long-line-reliability-prd.md. M0 requires an independently reproducible failure and measured allocation/time evidence before production changes. Static inspection alone is not reproduction.
- Keep baseline, candidate and diagnostic results separate. Record failures and unsupported modes explicitly; passing tests do not establish full acceptance or platform coverage.
- Preserve C++17, Qt Widgets and existing scheduling/index structures unless evidence requires a narrowly scoped change. Do not replace the product with a separate demonstration application.
- Subagents must not spawn descendants, commit, push, change Git identity, or publish. Each assigned writer owns only its named files. Preserve other workers' changes.
- Generated logs, large fixtures, build outputs and dependency caches belong under ignored local directories. Never commit credentials or machine-specific secrets.
