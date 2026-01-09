Generates Github Workflows for CI builders descriptions.

Example usage:

```bash
cd engine/src
dart flutter/tools/pkg/github_workflow_generator/bin/run.dart \
  -i flutter/ci/builders/mac_host_engine.json \
  -o ../../../.github/workflows/build-and-upload-engine-artifacts.yml
```

Multiple input files may be specified for single output file.
