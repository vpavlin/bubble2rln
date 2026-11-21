{
  description = "Bubble: pure-QML Basecamp view over the bubble_core module";

  inputs = {
    # Same builder as the core (Basecamp 0.3.x).
    logos-module-builder.url = "github:logos-co/logos-module-builder/0.3.1";
    # The core this view was built and tested with (a path:../core input can't be fetched by CI).
    bubble_core.url = "github:vpavlin/bubble2rln/84296f0d5f3d35298fc18e58bb15b833abf90bf7?dir=core";
    bubble_core.inputs.logos-module-builder.follows = "logos-module-builder";
  };

  outputs = inputs@{ logos-module-builder, ... }:
    logos-module-builder.lib.mkLogosQmlModule {
      src = ./.;
      configFile = ./metadata.json;
      flakeInputs = inputs;
    };
}
