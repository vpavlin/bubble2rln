{
  description = "Logos module: RLN membership allocation client over libp2p";

  inputs = {
    logos-module-builder.url = "github:logos-co/logos-module-builder/0.3.1";
    libp2p_module.url = "git+https://github.com/logos-co/logos-libp2p-module?ref=refs/tags/v1.1.1";
  };

  outputs = inputs@{ logos-module-builder, libp2p_module, ... }:
    let
      nixpkgs = logos-module-builder.inputs.nixpkgs;
      meta = builtins.fromJSON (builtins.readFile ./metadata.json);
      module = logos-module-builder.lib.mkLogosModule {
        src = ./.;
        configFile = ./metadata.json;
        flakeInputs = inputs;
      };
      devSystems = [ "aarch64-darwin" "x86_64-darwin" "aarch64-linux" "x86_64-linux" ];
    in
    module // {
      apps = nixpkgs.lib.genAttrs devSystems (system:
        let
          pkgs = import nixpkgs { inherit system; };
          lidlGen = logos-module-builder.inputs.logos-rust-sdk.packages.${system}.lidl-gen;
          sdkSrc = logos-module-builder.packages.${system}.rust-sdk-src;
          libp2pLidl = "${libp2p_module.packages.${system}.lidl}/libp2p_module.lidl";
          protocolVersion =
            let
              header = builtins.readFile
                "${logos-module-builder.inputs.logos-protocol}/cpp/logos_protocol.h";
              parts = builtins.split "LOGOS_PROTOCOL_VERSION_STRING \"([^\"]*)\"" header;
            in
            if builtins.length parts < 2
            then throw "logos-protocol header carries no LOGOS_PROTOCOL_VERSION_STRING"
            else builtins.head (builtins.elemAt parts 1);
          generate = pkgs.writeShellApplication {
            name = "membership-allocation-client-module-generate";
            runtimeInputs = [ lidlGen pkgs.gitMinimal ];
            text = ''
              root="''${1:-$(git rev-parse --show-toplevel)}"
              gen="$root/rust-lib/generated"
              echo "generating rust-lib/generated/provider_gen.rs (protocol ${protocolVersion}) ..."
              mkdir -p "$gen"
              logos-lidl-gen --from-rust "$root/rust-lib/src/lib.rs" \
                --trait MembershipAllocationClientModule --module-name ${meta.name} --module-version ${meta.version} \
                -o "$gen/${meta.name}.lidl"
              logos-lidl-gen "$gen/${meta.name}.lidl" --provider --no-trait \
                --dep libp2p_module=${libp2pLidl} \
                --protocol-version ${protocolVersion} \
                -o "$gen/provider_gen.rs"
              echo "staging the SDK source at logos-rust-sdk-src/ ..."
              rm -rf "''${root:?}/logos-rust-sdk-src"
              cp -RL "${sdkSrc}" "$root/logos-rust-sdk-src"
              chmod -R u+w "$root/logos-rust-sdk-src"
              echo "done. bare 'cargo build' now works in rust-lib/"
            '';
          };
        in
        (module.apps.${system} or { }) // {
          generate = {
            type = "app";
            program = "${generate}/bin/membership-allocation-client-module-generate";
          };
        });
    };
}
