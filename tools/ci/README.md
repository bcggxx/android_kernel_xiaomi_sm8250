# Dagu CI path guard

Run `python3 tools/ci/check_dagu_ci_paths.py` and
`python3 tools/ci/test_dagu_ci_paths.py` from this checkout.

`dagu-dependencies.json` lists source and interface files used by the dagu host
checks. When a test gains a new source/header dependency, add it here and ensure
`dagu-host-check.yml` covers it. The checker also rejects missing manifest files.
It accepts the workflow's literal paths and directory `/**` patterns, failing
closed if the paths block changes to unsupported YAML syntax.

The separate `dagu-ci-path-check.yml` runs on every pull request so missing host
workflow triggers cannot prevent this guard from running. This checks the
maintained dependency list, not automatic discovery of arbitrary C includes.
