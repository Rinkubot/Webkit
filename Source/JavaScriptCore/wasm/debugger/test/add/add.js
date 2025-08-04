var wasm_code = read('add.wasm', 'binary');
var wasm_module = new WebAssembly.Module(wasm_code);

var imports = {
    wasi_snapshot_preview1: {
        proc_exit: function (code) {
            print("Program exited with code:", code);
        }
    }
};

var instance = new WebAssembly.Instance(wasm_module, imports);
let add = instance.exports.add;

let iteration = 0;

for (; ;) {
    add(iteration, iteration+1);
    iteration += 1;
    if (iteration % 1e7 == 0)
        print("iteration=", iteration);
}



