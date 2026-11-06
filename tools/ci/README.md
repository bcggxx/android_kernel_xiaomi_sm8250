# SM8250 CI path guard

Run `python3 tools/ci/check_sm8250_ci_paths.py` and
`python3 tools/ci/test_sm8250_ci_paths.py` from this checkout.

`sm8250-dependencies.json` lists source and interface files used by the SM8250
host checks. When a test gains a new source/header dependency, add it here and ensure
`sm8250-host-check.yml` covers it. The checker also rejects missing manifest files.
It accepts the workflow's literal paths and directory `/**` patterns, failing
closed if the paths block changes to unsupported YAML syntax.

The separate `sm8250-ci-path-check.yml` runs on every pull request so missing host
workflow triggers cannot prevent this guard from running. This checks the
maintained dependency list, not automatic discovery of arbitrary C includes.
