{
  description = "Bubble: pure-QML Basecamp view over the bubble_core module";

  inputs = {
    # Same builder as the core (Basecamp 0.3.x).
    logos-module-builder.url = "github:logos-co/logos-module-builder/0.3.1";
    # Local while the repo is unpublished; switch to a pinned GitHub ref (?dir=core) before release:
    # a relative path lock breaks a clean clone on some Nix versions.
    bubble_core.url = "path:../core";
    bubble_core.inputs.logos-module-builder.follows = "logos-module-builder";
  };

  outputs = inputs@{ logos-module-builder, ... }:
    logos-module-builder.lib.mkLogosQmlModule {
      src = ./.;
      configFile = ./metadata.json;
      flakeInputs = inputs;
    };
}
