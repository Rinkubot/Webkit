// Fuzzing script for BiquadFilterNode.type type confusion vulnerability.

function getRandomString(length) {
    let result = '';
    let characters = 'ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789';
    let charactersLength = characters.length;
    for (let i = 0; i < length; i++) {
        result += characters.charAt(Math.floor(Math.random() * charactersLength));
    }
    return result;
}

function fuzz() {
    let audioCtx = new AudioContext();
    let biquadFilter = audioCtx.createBiquadFilter();

    for (let i = 0; i < 1000; i++) {
        let malicious_string = getRandomString(Math.floor(Math.random() * 100) + 1);
        try {
            biquadFilter.type = malicious_string;
        } catch (e) {
            // We expect errors, but a crash would confirm the vulnerability.
        }
    }
    console.log("Fuzzing complete. If the browser hasn't crashed, the vulnerability may not be triggerable in this way.");
}

fuzz();
