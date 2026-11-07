// node-redis against a running Kosh, through node-redis's own API. Run by
// test_clients.py; exits non-zero, with the reason, on the first failure.
//
// Usage: node node_redis_check.cjs <port> <resp 2|3> [password] [--expect-auth-failure]
// node-redis is found through NODE_PATH (CommonJS require: ES module imports
// ignore NODE_PATH).
"use strict";
const assert = require("node:assert/strict");
const { createClient } = require("redis");

async function main() {
    const [port, resp] = [Number(process.argv[2]), Number(process.argv[3])];
    const expectAuthFailure = process.argv.includes("--expect-auth-failure");
    const password = process.argv[4] && !process.argv[4].startsWith("--") ? process.argv[4] : undefined;
    const client = createClient({
        socket: { host: "127.0.0.1", port, reconnectStrategy: false },
        RESP: resp,
        password,
    });
    client.on("error", () => {});   // connection errors also reject the calls below

    if (expectAuthFailure) {
        await assert.rejects(client.connect(), /WRONGPASS|NOAUTH/);
        return;
    }
    await client.connect();
    // Start from no keys: test_clients.py runs this twice (RESP2, RESP3) on one server.
    // One key at a time: DEL takes one key (C5).
    for (const key of ["k", "t", "l", "s", "h", "m"]) await client.del(key);
    assert.equal(await client.ping(), "PONG");

    // Strings and keys
    assert.equal(await client.set("k", "v"), "OK");
    assert.equal(await client.get("k"), "v");
    assert.equal(await client.get("missing"), null);
    assert.equal(await client.sendCommand(["SET", "t", "v", "EX", "100"]), "OK");
    const ttl = await client.ttl("t");
    assert.ok(ttl >= 99 && ttl <= 100, `TTL ${ttl}`);
    assert.equal(await client.exists("k"), 1);
    assert.equal(await client.type("k"), "string");
    assert.equal(await client.expire("k", 50), 1);
    assert.equal(await client.del("k"), 1);

    // Lists
    assert.equal(await client.rPush("l", ["a", "b"]), 2);
    assert.equal(await client.lPush("l", "z"), 3);
    assert.deepEqual(await client.lRange("l", 0, -1), ["z", "a", "b"]);
    assert.equal(await client.lPop("l"), "z");
    assert.equal(await client.rPop("l"), "b");
    assert.equal(await client.lLen("l"), 1);

    // Sets (a RESP3 set, a RESP2 array: the same members either way)
    assert.equal(await client.sAdd("s", ["a", "b", "a"]), 2);
    assert.deepEqual((await client.sMembers("s")).sort(), ["a", "b"]);
    assert.equal(Number(await client.sIsMember("s", "a")), 1);
    assert.equal(await client.sRem("s", "a"), 1);
    assert.equal(await client.sCard("s"), 1);

    // Hashes (a RESP3 map, a RESP2 array: the same object either way)
    assert.equal(await client.hSet("h", { f: "1", g: "2" }), 2);
    assert.equal(await client.hGet("h", "f"), "1");
    assert.deepEqual({ ...(await client.hGetAll("h")) }, { f: "1", g: "2" });
    assert.equal(await client.hDel("h", "f"), 1);
    assert.equal(await client.hLen("h"), 1);

    // A transaction, and many commands in flight at once (node-redis pipelines them)
    assert.deepEqual(await client.multi().set("m", "1").get("m").exec(), ["OK", "1"]);
    const many = await Promise.all(Array.from({ length: 100 }, (_, i) => client.set(`p${i}`, String(i))));
    assert.ok(many.every((r) => r === "OK"));
    assert.equal(await client.get("p99"), "99");

    // Errors, and INFO
    await assert.rejects(client.lPush("h", "x"), /WRONGTYPE/);
    assert.match(await client.info(), /kosh_version:\d+\.\d+\.\d+/);

    await client.close();
}

main().then(() => console.log("ok"), (error) => {
    console.log(`FAILED: ${error.stack || error}`);
    process.exit(1);
});
