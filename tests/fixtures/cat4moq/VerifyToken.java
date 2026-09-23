import java.nio.file.Files;
import java.nio.file.Path;
import java.util.Base64;
import java.util.Map;

import org.red5.server.net.moq.auth.CatTokenParser;
import org.red5.server.net.moq.auth.CatTokenValidator;
import org.red5.server.net.moq.auth.MoqtAction;
import org.red5.server.net.moq.message.TrackNamespace;

/** Cross-check externally issued tokens using the actual Red5 validator. */
public class VerifyToken {
    public static void main(String[] args) throws Exception {
        boolean moqx = args[0].equals("moqx");
        String secret = Files.readString(Path.of(args[1])).trim();
        byte[] key = moqx ? CatTokenValidator.deriveMoqxKey(secret) : Base64.getDecoder().decode(secret);
        var validator = new CatTokenValidator(Map.of("interop", key), 0,
                moqx ? 65000 : CatTokenParser.DEFAULT_MOQT_CLAIM_KEY,
                moqx ? 65001 : CatTokenParser.DEFAULT_MOQT_REVAL_CLAIM_KEY, false, moqx);
        var result = validator.validate(Files.readAllBytes(Path.of(args[2])));
        if (result.isValid() != Boolean.parseBoolean(args[7])) {
            throw new AssertionError("Unexpected signature/expiry validity: " + result.getStatus());
        }
        boolean allowed = result.isValid() && result.getToken().permitsAction(
                MoqtAction.valueOf(args[3]), TrackNamespace.fromPath(args[4]), args[5]);
        if (allowed != Boolean.parseBoolean(args[6])) {
            throw new AssertionError("Unexpected authorization decision: " + result.getStatus());
        }
        System.out.println("PASS: " + args[3] + " allowed=" + allowed);
    }
}
