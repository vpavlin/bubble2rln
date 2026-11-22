{
  description = "Bubble: pure-QML Basecamp view over the bubble_core module";

  inputs = {
    # Same builder as the core (Basecamp 0.3.x).
    logos-module-builder.url = "github:logos-co/logos-module-builder/0.3.1";
    # The core this view was built and tested with (a path:../core input can't be fetched by CI).
    bubble_core.url = "github:vpavlin/bubble2rln/a65f0655e7d165ca41cb1ee2405c614b9917d362?dir=core";
    bubble_core.inputs.logos-module-builder.follows = "logos-module-builder";
    # The RLN membership allocator's client: the claim goes through it (and it brings libp2p_module).
    membership_allocation_client_module.url = "github:logos-co/logos-membership-allocation-client-module/d8286f4072197952d4f7b575fbc2b4894074ec22";
  };

  outputs = inputs@{ logos-module-builder, ... }:
    logos-module-builder.lib.mkLogosQmlModule {
      src = ./.;
      configFile = ./metadata.json;
      flakeInputs = inputs;
    };
}
