export HOME=/root/bubble-testnet/home LOGOSCTL_CONFIG_DIR=/root/bubble-testnet/cfg QT_QPA_PLATFORM=offscreen
export LEE_WALLET_HOME_DIR=/root/bubble-testnet/wallet LEE_WALLET_PASSWORD=""
export LEZ_RLN_PAYER=6xhp9WpQJmNi3oC34XeUcbhJbuUqEVRbjTNpB35ZMw8x REGISTRY=J3NXPWUspX2zqJU3tUbR6MHmm83aJc5AzvidEaBBKnat
export MEMBERSHIP_REGISTRY_MODULE=liblogos_lez_rln_module
export GIFTER_PORT=9101 MAX_PER_HOUR=6
# The payer's private key (BIP340 secret, 32-byte hex) is imported into the module's wallet at
# bring-up; it is not in this repo:
#   export LEZ_RLN_PAYER_KEY=$(cat /root/bubble-testnet/payer.ssk)
