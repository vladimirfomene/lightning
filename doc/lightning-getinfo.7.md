lightning-getinfo -- Command to receive all information about the Core Lightning node.
============================================================

SYNOPSIS
--------

**getinfo**

DESCRIPTION
-----------

The **getinfo** gives a summary of the current running node.


EXAMPLE JSON REQUEST
------------
```json
{
  "id": 82,
  "method": "getinfo",
  "params": {}
}
```

RETURN VALUE
------------

[comment]: # (GENERATE-FROM-SCHEMA-START)
On success, an object is returned, containing:
- **id** (pubkey): The public key unique to this node
- **alias** (string): The fun alias this node will advertize (up to 32 characters)
- **color** (hex): The favorite RGB color this node will advertize (always 6 characters)
- **num_peers** (u32): The total count of peers, connected or with channels
- **num_pending_channels** (u32): The total count of channels being opened
- **num_active_channels** (u32): The total count of channels in normal state
- **num_inactive_channels** (u32): The total count of channels waiting for opening or closing transactions to be mined
- **version** (string): Identifies what bugs you are running into
- **lightning-dir** (string): Identifies where you can find the configuration and other related files
- **blockheight** (u32): The highest block height we've learned
- **network** (string): represents the type of network on the node are working (e.g: `bitcoin`, `testnet`, or `regtest`)
- **fees_collected_msat** (msat): Total routing fees collected by this node
- **our_features** (object, optional): Our BOLT #9 feature bits (as hexstring) for various contexts:
  - **init** (hex): features (incl. globalfeatures) in our init message, these also restrict what we offer in open_channel or accept in accept_channel
  - **node** (hex): features in our node_announcement message
  - **channel** (hex): negotiated channel features we (as channel initiator) publish in the channel_announcement message
  - **invoice** (hex): features in our BOLT11 invoices
- **address** (array of objects, optional): The addresses we announce to the world:
  - **type** (string): Type of connection (one of "dns", "ipv4", "ipv6", "torv2", "torv3", "websocket")
  - **port** (u16): port number

  If **type** is "dns", "ipv4", "ipv6", "torv2" or "torv3":
    - **address** (string): address in expected format for **type**
- **binding** (array of objects, optional): The addresses we are listening on:
  - **type** (string): Type of connection (one of "local socket", "ipv4", "ipv6", "torv2", "torv3")
  - **address** (string, optional): address in expected format for **type**
  - **port** (u16, optional): port number
  - **socket** (string, optional): socket filename (only if **type** is "local socket")

The following warnings may also be returned:
- **warning_bitcoind_sync**: Bitcoind is not up-to-date with network.
- **warning_lightningd_sync**: Lightningd is still loading latest blocks from bitcoind.

[comment]: # (GENERATE-FROM-SCHEMA-END)

On failure, one of the following error codes may be returned:

- -32602: Error in given parameters or some error happened during the command process.

EXAMPLE JSON RESPONSE
-----
```json
{
   "id": "02bf811f7571754f0b51e6d41a8885f5561041a7b14fac093e4cffb95749de1a8d",
   "alias": "SLICKERGOPHER",
   "color": "02bf81",
   "num_peers": 0,
   "num_pending_channels": 0,
   "num_active_channels": 0,
   "num_inactive_channels": 0,
   "address": [
      {
         "type": "torv3",
         "address": "fp463inc4w3lamhhduytrwdwq6q6uzugtaeapylqfc43agrdnnqsheyd.onion",
         "port": 9736
      },
      {
         "type": "torv3",
         "address": "ifnntp5ak4homxrti2fp6ckyllaqcike447ilqfrgdw64ayrmkyashid.onion",
         "port": 9736
      }
   ],
   "binding": [
      {
         "type": "ipv4",
         "address": "127.0.0.1",
         "port": 9736
      }
   ],
   "version": "v0.10.2",
   "blockheight": 724302,
   "network": "bitcoin",
   "msatoshi_fees_collected": 0,
   "fees_collected_msat": "0msat",
   "lightning-dir": "/media/vincent/Maxtor/C-lightning/node/bitcoin"
   "our_features": {
      "init": "8828226aa2",
      "node": "80008828226aa2",
      "channel": "",
      "invoice": "20024200"
   }
}

```


AUTHOR
------

Vincenzo Palazzo <<vincenzo.palazzo@protonmail.com>> wrote the initial version of this man page, but many others did the hard work of actually implementing this rpc command.


SEE ALSO
------

lightning-connect(7), lightning-fundchannel(7), lightning-listconfigs(7).

RESOURCES
---------

Main web site: <https://github.com/ElementsProject/lightning>
[comment]: # ( SHA256STAMP:aedad2e6949f96d8d331e39c17effe5786816729562987b058d146b4b94286cb)
