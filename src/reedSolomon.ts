const GF_SIZE = 256;
const PRIMITIVE_POLY = 0x11d;
const gfExp = new Uint8Array(GF_SIZE * 2);
const gfLog = new Uint8Array(GF_SIZE);

(function initTables() {
  let x = 1;
  for (let i = 0; i < GF_SIZE - 1; i++) {
    gfExp[i] = x;
    gfLog[x] = i;
    x <<= 1;
    if (x & 0x100) x ^= PRIMITIVE_POLY;
  }
  for (let i = GF_SIZE - 1; i < gfExp.length; i++) {
    gfExp[i] = gfExp[i - (GF_SIZE - 1)];
  }
})();

function gfMul(a: number, b: number): number {
  if (a === 0 || b === 0) return 0;
  return gfExp[gfLog[a] + gfLog[b]];
}

function gfDiv(a: number, b: number): number {
  if (a === 0) return 0;
  if (b === 0) throw new Error("Division by zero");
  return gfExp[(gfLog[a] + (GF_SIZE - 1) - gfLog[b]) % (GF_SIZE - 1)];
}

function gfPow(a: number, power: number): number {
  return gfExp[(gfLog[a] * power) % (GF_SIZE - 1)];
}

function polyScale(p: number[], x: number): number[] {
  return p.map((c) => gfMul(c, x));
}

function polyAdd(p: number[], q: number[]): number[] {
  const length = Math.max(p.length, q.length);
  const result = new Array<number>(length).fill(0);
  for (let i = 0; i < length; i++) {
    const a = p[p.length - length + i] ?? 0;
    const b = q[q.length - length + i] ?? 0;
    result[i] = a ^ b;
  }
  return result;
}

function polyMul(p: number[], q: number[]): number[] {
  const result = new Array<number>(p.length + q.length - 1).fill(0);
  for (let i = 0; i < p.length; i++) {
    for (let j = 0; j < q.length; j++) {
      result[i + j] ^= gfMul(p[i], q[j]);
    }
  }
  return result;
}

function polyEval(p: number[], x: number): number {
  let y = p[0];
  for (let i = 1; i < p.length; i++) {
    y = gfMul(y, x) ^ p[i];
  }
  return y;
}

function rsGeneratorPoly(nsym: number): number[] {
  let g = [1];
  for (let i = 0; i < nsym; i++) {
    g = polyMul(g, [1, gfPow(2, i)]);
  }
  return g;
}

export function rsEncode(data: Uint8Array, nsym: number): Uint8Array {
  const gen = rsGeneratorPoly(nsym);
  const msg = new Array<number>(data.length + nsym).fill(0);
  for (let i = 0; i < data.length; i++) msg[i] = data[i];

  for (let i = 0; i < data.length; i++) {
    const coef = msg[i];
    if (coef !== 0) {
      for (let j = 0; j < gen.length; j++) {
        msg[i + j] ^= gfMul(gen[j], coef);
      }
    }
  }

  const parity = msg.slice(data.length);
  const codeword = new Uint8Array(data.length + nsym);
  codeword.set(data, 0);
  codeword.set(parity, data.length);
  return codeword;
}

export function rsDecode(
  codeword: Uint8Array,
  nsym: number
): { data: Uint8Array; corrected: boolean; errorCount: number } {
  const msg = Array.from(codeword);
  const synd = new Array<number>(nsym).fill(0);
  let hasError = false;

  for (let i = 0; i < nsym; i++) {
    const s = polyEval(msg, gfPow(2, i));
    synd[i] = s;
    if (s !== 0) hasError = true;
  }

  if (!hasError) {
    return { data: codeword.slice(0, codeword.length - nsym), corrected: true, errorCount: 0 };
  }

  let errLoc = [1];
  let oldLoc = [1];
  for (let i = 0; i < nsym; i++) {
    let delta = synd[i];
    for (let j = 1; j < errLoc.length; j++) {
      delta ^= gfMul(errLoc[errLoc.length - 1 - j], synd[i - j]);
    }
    oldLoc.push(0);
    if (delta !== 0) {
      if (oldLoc.length > errLoc.length) {
        const newLoc = polyScale(oldLoc, delta);
        oldLoc = polyScale(errLoc, gfDiv(1, delta));
        errLoc = newLoc;
      }
      errLoc = polyAdd(errLoc, polyScale(oldLoc, delta));
    }
  }

  const errPositions: number[] = [];
  for (let i = 0; i < msg.length; i++) {
    if (polyEval(errLoc, gfPow(2, i)) === 0) {
      errPositions.push(msg.length - 1 - i);
    }
  }

  if (errPositions.length === 0 || errPositions.length > nsym) {
    return { data: codeword.slice(0, codeword.length - nsym), corrected: false, errorCount: errPositions.length };
  }

  const syndPoly = [0].concat(synd);
  const errLocRev = errLoc.slice().reverse();
  let errEval = polyMul(syndPoly, errLocRev);
  errEval = errEval.slice(errEval.length - nsym);

  for (const pos of errPositions) {
    const xi = gfPow(2, msg.length - 1 - pos);
    let errLocPrime = 1;
    for (let j = 1; j < errLoc.length; j += 2) {
      errLocPrime ^= gfMul(errLoc[errLoc.length - 1 - j], gfPow(xi, j));
    }
    const y = polyEval(errEval, xi);
    const magnitude = gfDiv(gfMul(y, xi), errLocPrime);
    msg[pos] ^= magnitude;
  }

  return {
    data: Uint8Array.from(msg.slice(0, msg.length - nsym)),
    corrected: true,
    errorCount: errPositions.length,
  };
}
