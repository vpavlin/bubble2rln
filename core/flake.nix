{
  description = "bubble_core: Basecamp core module for Bubble (grow, digest, claim an RLN membership)";

  inputs = {
    # Basecamp 0.3.x: builder 0.3.1 (release tag).
    logos-module-builder.url = "github:logos-co/logos-module-builder/0.3.1";
    # The RLN wallet (LEZ testnet reads, the sync Bubble shows). A dependency so Basecamp loads it
    # with Bubble: a module can't load another, and without Chat nothing else would. Pinned at 5.1.0.
    liblogos_lez_rln_module.url = "github:logos-co/logos-rln-modules/d9f8b3d5ff93f6fd6fb363f5689098c07e81ac47?dir=logos-lez-rln-module";
    # The RLN keystore (the player's identity; the delegated claim goes through it). Delivery uses the
    # same module, so the membership backs Chat too; without Chat nothing else would load it. 0.12.0.
    liblogos_rln_module.url = "github:logos-co/logos-rln-modules/d9f8b3d5ff93f6fd6fb363f5689098c07e81ac47?dir=logos-rln-module";
    # Logos Delivery (the leaderboard's scores are posted and received on it). The version Chat uses.
    delivery_module.url = "github:logos-co/logos-delivery-module/v0.3.3";
  };

  outputs = inputs@{ logos-module-builder, ... }:
    logos-module-builder.lib.mkLogosModule {
      src = ./.;
      configFile = ./metadata.json;
      flakeInputs = inputs;
    };
}
