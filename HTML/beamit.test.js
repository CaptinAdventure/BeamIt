import assert from "node:assert/strict";
import { createHmac, pbkdf2Sync, webcrypto } from "node:crypto";
import test from "node:test";

import {
  base64UrlDecode,
  base64UrlEncode,
  computeAuthProofs,
  parseVideoFrame,
} from "./beamit.js";

test("base64url uses the unpadded RFC 4648 URL alphabet", () => {
  assert.equal(base64UrlEncode(new Uint8Array()), "");
  assert.equal(base64UrlEncode(new TextEncoder().encode("foobar")), "Zm9vYmFy");
  assert.equal(base64UrlEncode(Uint8Array.of(0xfb, 0xff)), "-_8");
  assert.deepEqual(base64UrlDecode("-_8"), Uint8Array.of(0xfb, 0xff));
  assert.throws(() => base64UrlDecode("Zm9v="), /Invalid base64url/);
  assert.throws(() => base64UrlDecode("Zh"), /Non-canonical base64url/);
});

test("authentication proofs match PBKDF2-HMAC-SHA256 protocol construction", async () => {
  const salt = new TextEncoder().encode("beamit-test-salt");
  const serverNonce = Uint8Array.from({ length: 32 }, (_, index) => index);
  const clientNonce = Uint8Array.from({ length: 32 }, (_, index) => 255 - index);
  const password = "correct horse battery staple";
  const iterations = 1200;
  const proofs = await computeAuthProofs(password, salt, iterations, serverNonce, clientNonce, webcrypto);
  const key = pbkdf2Sync(password, salt, iterations, 32, "sha256");
  const message = (role) => Buffer.concat([
    Buffer.from(`BeamIt v1 ${role}\0`, "utf8"),
    Buffer.from(serverNonce),
    Buffer.from(clientNonce),
  ]);

  assert.deepEqual(Buffer.from(proofs.clientProof), createHmac("sha256", key).update(message("client")).digest());
  assert.deepEqual(Buffer.from(proofs.serverProof), createHmac("sha256", key).update(message("server")).digest());
  assert.notDeepEqual(proofs.clientProof, proofs.serverProof);
});

test("video frame parser honors header length, network byte order, and flags", () => {
  const bytes = new Uint8Array(41);
  const view = new DataView(bytes.buffer);
  view.setUint8(0, 1);
  view.setUint8(1, 1);
  view.setUint16(2, 36);
  view.setUint32(4, 0x80000003);
  view.setUint32(8, 7);
  view.setBigUint64(16, 0x0102030405060708n);
  view.setBigUint64(24, 0x1112131415161718n);
  bytes.set([1, 2, 3, 4, 5], 36);

  const frame = parseVideoFrame(bytes);
  assert.equal(frame.keyFrame, true);
  assert.equal(frame.discontinuity, true);
  assert.equal(frame.flags, 0x80000003);
  assert.equal(frame.generation, 7);
  assert.equal(frame.sequence, 0x0102030405060708n);
  assert.equal(frame.captureTimeUs, 0x1112131415161718n);
  assert.deepEqual(frame.payload, Uint8Array.of(1, 2, 3, 4, 5));
});

test("video frame parser rejects malformed headers and ignores unknown message types", () => {
  assert.throws(() => parseVideoFrame(new Uint8Array([1, 1, 0, 31])), /Truncated|Invalid/);
  const invalidHeader = new Uint8Array(33);
  invalidHeader[0] = 1;
  invalidHeader[1] = 1;
  new DataView(invalidHeader.buffer).setUint16(2, 31);
  assert.throws(() => parseVideoFrame(invalidHeader), /header length/);

  assert.equal(parseVideoFrame(Uint8Array.of(1, 2)), null);
});
