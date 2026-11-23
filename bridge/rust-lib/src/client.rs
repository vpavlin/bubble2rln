use membership_allocation_action::{
    self as action, commitment_refusal, Grant, ACTION_KIND, AUTH_DOMAIN, CODEC, COMMITMENT_LEN,
};
use paymaster_client::{self as engine, ClientAction, Route};
use paymaster_wire::canonical_account;
use paymaster_wire::envelope::SponsorRequest;
use serde_json::{json, Value};

const PEER_KEY: &str = "allocatorPeerId";

struct Membership<'a> {
    commitment_hex: &'a str,
    registry: Option<&'a str>,
}

impl ClientAction for Membership<'_> {
    const CODEC: &'static str = CODEC;
    const PROVIDER: &'static str = "allocator";

    fn grant(&self, result: &[u8], peer_id: &str) -> Result<Value, String> {
        let grant = Grant::decode(result)?;
        if let Some(want) = self.registry {
            if !grant.registry.eq_ignore_ascii_case(want) {
                return Err(format!(
                    "allocator registered on {}, not the requested {want}",
                    grant.registry
                ));
            }
        }
        let mut out = json!({
            "leaf_index": grant.leaf_index,
            "id_commitment": self.commitment_hex,
            "config_account": grant.registry,
            PEER_KEY: peer_id,
        });
        if !grant.tx_hash.is_empty() {
            out["tx_hash"] = json!(grant.tx_hash);
        }
        Ok(out)
    }
}

fn failed(kind: &str, error: impl Into<String>) -> Value {
    json!({ "kind": kind, "error": error.into() })
}

fn invalid(error: impl Into<String>) -> Value {
    failed("invalid_request", error)
}

fn text<'a>(args: &'a Value, key: &str) -> Option<&'a str> {
    args.get(key)
        .and_then(Value::as_str)
        .map(str::trim)
        .filter(|s| !s.is_empty())
}

pub fn discover(args_json: &str) -> Result<Value, Value> {
    let a: Value =
        serde_json::from_str(args_json).map_err(|e| invalid(format!("discover args: {e}")))?;
    let registry =
        text(&a, "config").ok_or_else(|| invalid("discover needs config (the registry)"))?;
    let registry = canonical_account(registry).map_err(|e| invalid(format!("config: {e}")))?;
    let auth_type = a.get("authType").and_then(Value::as_str).unwrap_or("");
    engine::discover::<Membership<'_>>(&registry, auth_type).map_err(|e| failed("no_provider", e))
}

pub fn request(args_json: &str) -> Result<Value, Value> {
    let a: Value =
        serde_json::from_str(args_json).map_err(|e| invalid(format!("request args: {e}")))?;
    let configured = match (text(&a, "allocatorPeerId"), text(&a, "allocatorMultiaddr")) {
        (Some(p), Some(m)) => Some((p, m)),
        (None, None) => None,
        _ => {
            return Err(invalid(
                "allocatorPeerId and allocatorMultiaddr go together",
            ))
        }
    };
    let discover = match a.get("discover") {
        None | Some(Value::Null) => true,
        Some(Value::Bool(b)) => *b,
        Some(_) => return Err(invalid("discover must be a boolean")),
    };
    if !discover && configured.is_none() {
        return Err(invalid(
            "request needs discover or allocatorPeerId+allocatorMultiaddr",
        ));
    }
    let registry = match text(&a, "config") {
        Some(c) => Some(canonical_account(c).map_err(|e| invalid(format!("config: {e}")))?),
        None => None,
    };
    if discover && registry.is_none() && configured.is_none() {
        return Err(invalid(
            "discovery needs config (the registry); name one or configure allocatorPeerId+allocatorMultiaddr",
        ));
    }

    let commitment_hex = text(&a, "identityCommitment")
        .ok_or_else(|| invalid("request needs identityCommitment"))?
        .trim_start_matches("0x")
        .to_lowercase();
    let commitment = hex::decode(&commitment_hex)
        .map_err(|e| invalid(format!("identityCommitment hex: {e}")))?;
    if let Some(why) = commitment_refusal(&commitment) {
        return Err(invalid(why));
    }
    let mut commitment_bytes = [0u8; COMMITMENT_LEN];
    commitment_bytes.copy_from_slice(&commitment);
    let rate = a.get("rate").and_then(Value::as_u64).filter(|r| *r > 0);

    let mut req = SponsorRequest {
        request_id: format!("membership-{}", &commitment_hex[..16]),
        authentication_type: Vec::new(),
        authentication_payload: Vec::new(),
        action_kind: ACTION_KIND.to_owned(),
        action: action::encode(&commitment_bytes, rate),
        scope: registry,
    };
    let (auth_type, auth_payload) = engine::auth_payload(&a, AUTH_DOMAIN, &req.action_digest())
        .map_err(|e| {
            let kind = match (text(&a, "authProvider"), text(&a, "authPayload")) {
                (Some(_), None) if text(&a, "authType").is_some() => "auth_failed",
                _ => "invalid_request",
            };
            failed(kind, e)
        })?;
    req.authentication_type = auth_type.as_bytes().to_vec();
    req.authentication_payload = auth_payload;

    let route = Route {
        discover,
        configured,
        spread: commitment_bytes[0] as usize,
    };
    engine::request(
        &Membership {
            commitment_hex: &commitment_hex,
            registry: req.scope.as_deref(),
        },
        &route,
        &req,
    )
    .map_err(|f| f.json(PEER_KEY))
}

#[cfg(test)]
mod tests {
    use paymaster_wire::envelope::{SponsorResponse, Status};
    use paymaster_wire::service_data;

    use super::*;
    use crate::lp::{self, b64};

    fn canonical() -> String {
        format!("{}0a", "ab".repeat(31))
    }

    fn registry() -> String {
        hex::encode([0xd; 32])
    }

    fn args(extra: &Value) -> String {
        let mut a = json!({
            "identityCommitment": canonical(),
            "rate": 100,
            "discover": false,
            "allocatorPeerId": "G",
            "allocatorMultiaddr": "/ip4/127.0.0.1/tcp/1",
        });
        for (k, v) in extra.as_object().into_iter().flatten() {
            if v.is_null() {
                a.as_object_mut().unwrap().remove(k);
            } else {
                a[k] = v.clone();
            }
        }
        a.to_string()
    }

    fn discovering(extra: &Value) -> String {
        let mut a = json!({ "identityCommitment": canonical(), "config": registry() });
        for (k, v) in extra.as_object().into_iter().flatten() {
            a[k] = v.clone();
        }
        a.to_string()
    }

    type Answer = fn(&SponsorRequest) -> Result<SponsorResponse, String>;

    fn scripted(peers: usize, records: Value, answer: Answer) {
        lp::fake::install(Box::new(move |_, method, a| match method {
            "discoRegisterInterest" => Ok(json!(true)),
            "connectedPeers" => Ok(json!((0..peers)
                .map(|i| format!("P{i}"))
                .collect::<Vec<_>>())),
            "discoLookup" => Ok(records.clone()),
            "protocolRequest" => {
                let pr: Value = serde_json::from_str(a[0].as_str().unwrap()).unwrap();
                assert_eq!(pr["proto"], CODEC);
                let frame = lp::b64_decode(pr["requestB64"].as_str().unwrap()).unwrap();
                let req = SponsorRequest::decode(&frame).unwrap();
                answer(&req).map(|resp| json!({ "responseB64": b64(&resp.encode()) }))
            }
            auth_vector::PRODUCE_METHOD => {
                let req: Value = serde_json::from_str(a[0].as_str().unwrap()).unwrap();
                assert_eq!(req["domain"], AUTH_DOMAIN);
                Ok(json!({ "payload_hex": "c0de" }))
            }
            m => Err(format!("unscripted {m}")),
        }));
    }

    fn advertising(peer: &str, auth_type: &str) -> Value {
        let data = service_data(&[(registry().as_str(), auth_type)]).unwrap();
        json!([{ "peerId": peer, "seqNo": 1, "addrs": ["/ip4/127.0.0.1/tcp/1"],
                 "services": [{ "id": CODEC, "data": b64(data.as_bytes()) }] }])
    }

    fn reply(req: &SponsorRequest, status: Status, message: Option<&str>) -> SponsorResponse {
        SponsorResponse {
            request_id: req.request_id.clone(),
            status,
            message: message.map(str::to_owned),
            result: None,
            code: None,
        }
    }

    fn granting(req: &SponsorRequest) -> Result<SponsorResponse, String> {
        assert_eq!(req.action_kind, ACTION_KIND);
        let (commitment, rate) = action::split(&req.action).unwrap();
        assert_eq!(hex::encode(commitment), canonical());
        assert!(matches!(rate, None | Some(100)), "{rate:?}");
        Ok(SponsorResponse {
            result: Some(
                Grant {
                    leaf_index: 4,
                    tx_hash: "7e".into(),
                    registry: req.scope.clone().unwrap_or_else(registry),
                }
                .encode(),
            ),
            ..reply(req, Status::Granted, None)
        })
    }

    fn granting_elsewhere(req: &SponsorRequest) -> Result<SponsorResponse, String> {
        Ok(SponsorResponse {
            result: Some(
                Grant {
                    leaf_index: 1,
                    tx_hash: String::new(),
                    registry: "bb".repeat(32),
                }
                .encode(),
            ),
            ..reply(req, Status::Granted, None)
        })
    }

    fn refusing(req: &SponsorRequest) -> Result<SponsorResponse, String> {
        Ok(SponsorResponse {
            code: Some("credential_used".into()),
            ..reply(req, Status::Refused, Some("credential already used: x"))
        })
    }

    fn not_submitting(req: &SponsorRequest) -> Result<SponsorResponse, String> {
        Ok(reply(
            req,
            Status::NotSubmitted,
            Some("register_member refused: insufficient_balance"),
        ))
    }

    fn unconfirming(req: &SponsorRequest) -> Result<SponsorResponse, String> {
        Ok(reply(req, Status::Unconfirmed, Some("not on chain")))
    }

    fn undialable(_: &SponsorRequest) -> Result<SponsorResponse, String> {
        Err("protocolRequest: dial failed".into())
    }

    #[test]
    fn bad_arguments_are_refused_before_any_io() {
        lp::fake::install(Box::new(|_, m, _| Err(format!("unscripted {m}"))));
        for (extra, needle) in [
            (
                json!({ "identityCommitment": "ff".repeat(32) }),
                "invalid_commitment: not a BN254",
            ),
            (
                json!({ "identityCommitment": "00".repeat(32) }),
                "invalid_commitment: zero",
            ),
            (json!({ "identityCommitment": "ab".repeat(31) }), "32 bytes"),
            (
                json!({ "identityCommitment": "zz" }),
                "identityCommitment hex",
            ),
            (
                json!({ "identityCommitment": null }),
                "needs identityCommitment",
            ),
            (json!({ "config": "nope" }), "config: account 'nope'"),
            (json!({ "allocatorMultiaddr": null }), "go together"),
            (
                json!({ "allocatorPeerId": null, "allocatorMultiaddr": null }),
                "needs discover or allocatorPeerId",
            ),
            (
                json!({ "discover": true, "allocatorPeerId": null, "allocatorMultiaddr": null }),
                "discovery needs config",
            ),
            (json!({ "discover": "no" }), "discover must be a boolean"),
            (
                json!({ "authType": "t", "authPayload": "aa", "authProvider": "m" }),
                "mutually exclusive",
            ),
            (json!({ "authPayload": "aa" }), "need an explicit authType"),
            (json!({ "authProvider": "m" }), "need an explicit authType"),
            (
                json!({ "authType": "t", "authPayload": "zz" }),
                "authPayload hex",
            ),
        ] {
            let f = request(&args(&extra)).unwrap_err();
            assert_eq!(f["kind"], "invalid_request", "{extra}: {f}");
            assert!(
                f["error"].as_str().unwrap().contains(needle),
                "{extra}: {f}"
            );
        }
        assert!(lp::fake::calls().is_empty(), "{:?}", lp::fake::methods());
        let f = discover("{}").unwrap_err();
        assert_eq!(f["kind"], "invalid_request");
        assert!(f["error"]
            .as_str()
            .unwrap()
            .contains("discover needs config"));
        assert!(discover(r#"{"config":"nope"}"#).unwrap_err()["error"]
            .as_str()
            .unwrap()
            .contains("config: account 'nope'"));
        lp::fake::clear();
    }

    #[test]
    fn a_grant_names_the_leaf_the_registry_and_the_allocator() {
        scripted(1, json!([]), granting);
        assert_eq!(
            request(&args(&json!({
                "identityCommitment": format!("0x{}", canonical().to_uppercase()),
                "config": registry().to_uppercase(),
            })))
            .unwrap(),
            json!({
                "leaf_index": 4,
                "id_commitment": canonical(),
                "config_account": registry(),
                "tx_hash": "7e",
                "allocatorPeerId": "G",
                "discovered": false,
            })
        );
        fn peer_any(req: &SponsorRequest) -> Result<SponsorResponse, String> {
            assert_eq!(req.authentication_type, b"peer-any");
            assert!(req.authentication_payload.is_empty());
            granting(req)
        }
        scripted(1, advertising("A", "peer-any"), peer_any);
        let granted = request(&discovering(&json!({ "authType": "peer-any" }))).unwrap();
        assert_eq!(granted["allocatorPeerId"], "A");
        assert_eq!(granted["discovered"], true);
        fn produced(req: &SponsorRequest) -> Result<SponsorResponse, String> {
            assert_eq!(req.authentication_payload, [0xc0, 0xde]);
            granting(req)
        }
        scripted(1, json!([]), produced);
        request(&args(&json!({ "authType": "t", "authProvider": "signer" }))).unwrap();
        scripted(1, json!([]), granting);
        assert_eq!(
            request(&args(&json!({}))).unwrap()["config_account"],
            registry()
        );
        lp::fake::clear();
    }

    #[test]
    fn a_failure_says_what_happened_in_a_field() {
        scripted(1, json!([]), refusing);
        assert_eq!(
            request(&args(&json!({}))).unwrap_err(),
            json!({
                "kind": "refused",
                "reason": "credential_used",
                "error": "credential already used: x",
                "allocatorPeerId": "G",
                "discovered": false,
            })
        );

        scripted(1, json!([]), not_submitting);
        let f = request(&args(&json!({}))).unwrap_err();
        assert_eq!(f["kind"], "not_submitted", "{f}");
        assert_eq!(f["allocatorPeerId"], "G");

        scripted(1, advertising("A", ""), unconfirming);
        assert_eq!(
            request(&discovering(&json!({}))).unwrap_err(),
            json!({
                "kind": "unconfirmed",
                "error": "A: not on chain",
                "allocatorPeerId": "A",
                "discovered": true,
                "unconfirmed": true,
            })
        );

        scripted(1, json!([]), granting_elsewhere);
        let f = request(&args(&json!({ "config": registry() }))).unwrap_err();
        assert_eq!(f["kind"], "failed", "{f}");
        assert_eq!(
            f["error"],
            format!(
                "allocator registered on {}, not the requested {}",
                "bb".repeat(32),
                registry()
            )
        );

        scripted(1, advertising("A", ""), undialable);
        assert_eq!(
            request(&discovering(&json!({}))).unwrap_err(),
            json!({
                "kind": "no_provider",
                "error": "no allocator granted and none is configured: A: protocolRequest: dial failed",
            })
        );
        lp::fake::install(Box::new(|_, m, _| Err(format!("{m}: libp2p down"))));
        let f = request(&discovering(&json!({}))).unwrap_err();
        assert_eq!(f["discovery"], "unavailable");
        assert_eq!(
            discover(&discovering(&json!({}))).unwrap_err()["kind"],
            "no_provider"
        );

        let f = request(&args(&json!({ "authType": "t", "authProvider": "signer" }))).unwrap_err();
        assert_eq!(f["kind"], "auth_failed", "{f}");
        lp::fake::clear();
    }

    #[test]
    fn discover_lists_the_allocators_advertising_the_pair() {
        scripted(1, advertising("A", "peer-any"), granting);
        assert_eq!(
            discover(&json!({ "config": registry(), "authType": "peer-any" }).to_string()).unwrap(),
            json!({
                "serviceId": CODEC,
                "candidates": [{ "peerId": "A", "addrs": ["/ip4/127.0.0.1/tcp/1"] }],
                "joined": true,
                "peers": 1,
            })
        );
        assert_eq!(
            discover(&json!({ "config": registry() }).to_string()).unwrap()["candidates"],
            json!([])
        );
        lp::fake::clear();
    }
}
