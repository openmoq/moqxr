import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.security.MessageDigest;
import java.util.Arrays;
import javax.crypto.Mac;
import javax.crypto.spec.SecretKeySpec;
import com.upokecenter.cbor.CBORObject;
import com.upokecenter.cbor.CBORType;

/** Controlled C4M-01 claim fixture, not a production CAT verifier.
 * Uses private labels -65537/-65538 because the draft still specifies TBD.
 * Implements only a pinned HMAC/COSE_Mac0 test profile; DPoP/revalidation fail
 * closed. Current relay profile acceptance is deliberately not required.
 */
public final class VerifyC4m01 {
    private static final int MOQ = -65537;
    private static final int REVAL = -65538;
    private static final byte[] KEY = "controlled-c4m01-fixture-key-32bytes".getBytes(StandardCharsets.UTF_8);
    private static final long NOW = 1800000000L;
    private static int decisions;

    private static byte[] bytes(String value) { return value.getBytes(StandardCharsets.UTF_8); }
    private static CBORObject array(Object... values) {
        CBORObject out = CBORObject.NewArray();
        for (Object value : values) out.Add(value);
        return out;
    }
    private static CBORObject claims(CBORObject scopes) {
        return CBORObject.NewMap().Add(1, "fixture").Add(3, "fixture-relay")
            .Add(4, NOW + 60).Add(5, NOW - 1).Add(MOQ, scopes);
    }
    private static byte[] mac(byte[] protectedBytes, byte[] payload) throws Exception {
        byte[] input = array("MAC0", protectedBytes, new byte[0], payload).EncodeToBytes();
        Mac hmac = Mac.getInstance("HmacSHA256");
        hmac.init(new SecretKeySpec(KEY, "HmacSHA256"));
        return hmac.doFinal(input);
    }
    private static byte[] issue(CBORObject claims) throws Exception {
        byte[] header = CBORObject.NewMap().Add(1, 5).Add(4, bytes("fixture")).EncodeToBytes();
        byte[] payload = claims.EncodeToBytes();
        return array(header, CBORObject.NewMap(), payload, mac(header, payload)).EncodeToBytes();
    }
    private static boolean binaryMatch(CBORObject pattern, byte[] value) {
        if (pattern.getType() == CBORType.ByteString)
            return Arrays.equals(pattern.GetByteString(), value);
        if (pattern.getType() != CBORType.Array || pattern.size() != 2 ||
            pattern.get(0).getType() != CBORType.Integer ||
            pattern.get(1).getType() != CBORType.ByteString) throw new IllegalArgumentException();
        int kind = pattern.get(0).AsInt32();
        byte[] needle = pattern.get(1).GetByteString();
        if (kind != 1 && kind != 2) throw new IllegalArgumentException();
        if (needle.length > value.length) return false;
        int offset = kind == 1 ? 0 : value.length - needle.length;
        return Arrays.equals(needle, Arrays.copyOfRange(value, offset, offset + needle.length));
    }
    private static boolean permits(byte[] token, int type, int action, byte[][] ns, byte[] name) {
        try {
            if (type != 1) return false;
            CBORObject cose = CBORObject.DecodeFromBytes(token);
            if (cose.getType() != CBORType.Array || cose.size() != 4) return false;
            byte[] headerBytes = cose.get(0).GetByteString(), payload = cose.get(2).GetByteString();
            CBORObject header = CBORObject.DecodeFromBytes(headerBytes);
            if (header.get(1).AsInt32() != 5 ||
                !Arrays.equals(header.get(4).GetByteString(), bytes("fixture")) ||
                !MessageDigest.isEqual(cose.get(3).GetByteString(), mac(headerBytes, payload))) return false;
            CBORObject claims = CBORObject.DecodeFromBytes(payload);
            if (!claims.get(1).AsString().equals("fixture") ||
                !claims.get(3).AsString().equals("fixture-relay") ||
                claims.get(4).AsInt64() <= NOW || claims.get(5).AsInt64() > NOW ||
                claims.ContainsKey(REVAL) || claims.ContainsKey(8)) return false;
            CBORObject scopes = claims.get(MOQ);
            if (scopes == null || scopes.getType() != CBORType.Array || scopes.size() == 0) return false;
            boolean allowed = false;
            for (int i = 0; i < scopes.size(); ++i) {
                CBORObject scope = scopes.get(i);
                if (scope.getType() != CBORType.Array || scope.size() < 1 || scope.size() > 3)
                    return false;
                CBORObject actions = scope.get(0);
                if (actions.getType() != CBORType.Array || actions.size() == 0) return false;
                boolean match = false;
                for (int j = 0; j < actions.size(); ++j)
                    if (actions.get(j).AsInt32() == action) match = true;
                if (scope.size() > 1) {
                    CBORObject patterns = scope.get(1);
                    if (patterns.getType() != CBORType.Array || patterns.size() == 0) return false;
                    for (int j = 0; j < patterns.size(); ++j) {
                        CBORObject pattern = patterns.get(j);
                        if (pattern.equals(CBORObject.Null)) {
                            if (j != patterns.size() - 1) return false;
                            match &= ns.length == j;
                        } else {
                            // Validate every pattern, even in a nonmatching scope.
                            boolean fieldMatch = binaryMatch(pattern, j < ns.length ? ns[j] : new byte[0]);
                            match &= j < ns.length && fieldMatch;
                        }
                    }
                }
                if (scope.size() > 2) match &= binaryMatch(scope.get(2), name);
                allowed |= match;
            }
            return allowed;
        } catch (Exception invalid) { return false; }
    }
    private static void check(boolean expected, byte[] token, int type, int action,
                              byte[][] ns, byte[] track) {
        ++decisions;
        if (permits(token, type, action, ns, track) != expected)
            throw new AssertionError("C4M fixture decision " + decisions);
    }
    public static void main(String[] args) throws Exception {
        byte[][] ns = {bytes("a/b"), bytes("c")};
        byte[] name = {0, 'v'};
        CBORObject exact = claims(array(array(array(6), array(bytes("a/b"), bytes("c"), CBORObject.Null), name)));
        byte[] token = issue(exact);
        Files.write(Path.of(args[0]), token);
        check(true, token, 1, 6, ns, name);
        check(false, token, 16, 6, ns, name);
        check(false, token, 1, 4, ns, name);
        check(false, token, 1, 6, new byte[][] {bytes("a"), bytes("b"), bytes("c")}, name);
        check(false, token, 1, 6, new byte[][] {bytes("a/b"), bytes("c"), bytes("")}, name);
        check(false, token, 1, 6, ns, bytes("v"));
        check(false, token, 1, 6, new byte[][] {bytes("a/b")}, name);
        byte[] corrupt = token.clone(); corrupt[corrupt.length - 1] ^= 1;
        check(false, corrupt, 1, 6, ns, name);
        exact.set(CBORObject.FromObject(4), CBORObject.FromObject(NOW));
        check(false, issue(exact), 1, 6, ns, name);
        exact.set(CBORObject.FromObject(4), CBORObject.FromObject(NOW + 60));
        exact.Add(REVAL, 1);
        check(false, issue(exact), 1, 6, ns, name);
        CBORObject prefix = claims(array(array(array(6), array(array(1, bytes("a"))), array(2, bytes("v")))));
        check(true, issue(prefix), 1, 6, ns, name);
        check(false, issue(prefix), 1, 6, new byte[][] {bytes("xa")}, name);
        check(false, issue(prefix), 1, 6, ns, bytes("vx"));
        check(true, issue(claims(array(array(array(0))))), 1, 0, new byte[0][], new byte[0]);
        check(true, issue(claims(array(array(array(6), array(bytes("a/b")))))), 1, 6, ns, name);
        check(false, issue(claims(array(array(array(6), array(CBORObject.Null, bytes("a/b")))))), 1, 6, ns, name);
        check(false, issue(claims(array(array(array(6), array("a/b", "c"))))), 1, 6, ns, name);
        check(false, issue(claims(array(array(array(6), array(array(0, bytes("a/b"))))))), 1, 6, ns, name);
        System.out.println("PASS: C4M-01 controlled fixture: " + decisions + " MAC/type/claim decisions; private labels -65537/-65538");
    }
}
