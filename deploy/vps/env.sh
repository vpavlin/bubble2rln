export HOME=/root/bubble-gifter/home LOGOSCTL_CONFIG_DIR=/root/bubble-gifter/cfg QT_QPA_PLATFORM=offscreen
export LEE_WALLET_HOME_DIR=/root/bubble-gifter/chain/wallet LEE_WALLET_PASSWORD=""
export LEZ_RLN_SEQUENCER=http://127.0.0.1:3040/
. /root/bubble-gifter/chain/registry.env
export LEZ_RLN_PAYER=$PAYER REGISTRY=$CONFIG_ACCOUNT
export GIFTER_PORT=9101 MAX_PER_HOUR=20
