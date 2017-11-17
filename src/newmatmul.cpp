#include <cstddef>
#include <tuple>
#include "newmatmul.h"
#include <NTL/BasicThreadPool.h>


/********************************************************************/
/****************** Auxiliary stuff: should go elsewhere   **********/



// FIXME: this is copied verbatim comes from Ctxt.cpp
// Compute the number of digits that we need and the esitmated
// added noise from switching this ciphertext part.
static std::pair<long, NTL::xdouble>
computeKSNoise(const CtxtPart& p, const FHEPubKey& pubKey, long pSpace)
{
  const FHEcontext& context = p.getContext();
  long nDigits = 0;
  xdouble addedNoise = to_xdouble(0.0);
  double sizeLeft = context.logOfProduct(p.getIndexSet());
  for (size_t i=0; i<context.digits.size() && sizeLeft>0.0; i++) {    
    nDigits++;

    double digitSize = context.logOfProduct(context.digits[i]);
    if (sizeLeft<digitSize) digitSize=sizeLeft;// need only part of this digit

    // Added noise due to this digit is phi(m) *sigma^2 *pSpace^2 *|Di|^2/4, 
    // where |Di| is the magnitude of the digit

    // WARNING: the following line is written just so to prevent overflow
    addedNoise += to_xdouble(context.zMStar.getPhiM()) * pSpace*pSpace
      * xexp(2*digitSize) * context.stdev*context.stdev / 4.0;

    sizeLeft -= digitSize;
  }

  // Sanity-check: make sure that the added noise is not more than the special
  // primes can handle: After dividing the added noise by the product of all
  // the special primes, it should be smaller than the added noise term due
  // to modulus switching, i.e., keyWeight * phi(m) * pSpace^2 / 12

  long keyWeight = pubKey.getSKeyWeight(p.skHandle.getSecretKeyID());
  double phim = context.zMStar.getPhiM();
  double logModSwitchNoise = log((double)keyWeight) 
    +2*log((double)pSpace) +log(phim) -log(12.0);
  double logKeySwitchNoise = log(addedNoise) 
    -2*context.logOfProduct(context.specialPrimes);
  assert(logKeySwitchNoise < logModSwitchNoise);

  return std::pair<long, NTL::xdouble>(nDigits,addedNoise);
}

class BasicAutomorphPrecon {
  Ctxt ctxt;
  NTL::xdouble noise;
  std::vector<DoubleCRT> polyDigits;

public:
  BasicAutomorphPrecon(const Ctxt& _ctxt) : ctxt(_ctxt)
  {
    FHE_TIMER_START;
    ctxt.cleanUp();

    const FHEcontext& context = ctxt.getContext();
    const FHEPubKey& pubKey = ctxt.getPubKey();
    long keyID = ctxt.getKeyID();

    // The call to cleanUp() should ensure that these assertions pass.

    assert(ctxt.parts.size() == 2); 
    assert(ctxt.parts[0].skHandle.isOne());
    assert(ctxt.parts[1].skHandle.isBase(keyID));
    assert(ctxt.getPrimeSet().disjointFrom(context.specialPrimes));
    

    // Compute the number of digits that we need and the esitmated
    // added noise from switching this ciphertext.
    long nDigits;
    std::tie(nDigits, noise)
      = computeKSNoise(ctxt.parts[1], pubKey, pubKey.keySWlist().at(0).ptxtSpace);

    double logProd = context.logOfProduct(context.specialPrimes);
    noise += ctxt.getNoiseVar() * xexp(2*logProd);

    // Break the ciphertext part into digits, if needed, and scale up these
    // digits using the special primes.

    ctxt.parts[1].breakIntoDigits(polyDigits, nDigits);
  }

  
  shared_ptr<Ctxt>
  automorph(long k) const
  {
    FHE_TIMER_START;

    if (k == 1) return make_shared<Ctxt>(ctxt);

    const FHEcontext& context = ctxt.getContext();
    const FHEPubKey& pubKey = ctxt.getPubKey();
    long keyID = ctxt.getKeyID();

    assert(pubKey.haveKeySWmatrix(1,k,keyID,keyID));
    const KeySwitch& W = pubKey.getKeySWmatrix(1,k,keyID,keyID);

    shared_ptr<Ctxt> result = make_shared<Ctxt>(Ctxt(ZeroCtxtLike, ctxt));

    result->noiseVar = noise; // noise estimate


    // Add in the constant part
    CtxtPart tmpPart = ctxt.parts[0];
    tmpPart.automorph(k);
    tmpPart.addPrimesAndScale(context.specialPrimes);
    result->addPart(tmpPart, /*matchPrimeSet=*/true);

    // "rotate" the digits before key-switching them
    vector<DoubleCRT> tmpDigits = polyDigits;
    for (auto&& tmp: tmpDigits) // rotate each of the digits
      tmp.automorph(k);

    result->keySwitchDigits(W, tmpDigits);

    return result;
  }
};


class GeneralAutomorphPrecon {
public:
  virtual ~GeneralAutomorphPrecon() {}

  virtual shared_ptr<Ctxt> automorph(long i) const = 0;

};

class GeneralAutomorphPrecon_UNKNOWN : public GeneralAutomorphPrecon {
private:
  Ctxt ctxt;
  long dim;
  const PAlgebra& zMStar;

public:
  GeneralAutomorphPrecon_UNKNOWN(const Ctxt& _ctxt, long _dim) :
    ctxt(_ctxt), dim(_dim), zMStar(_ctxt.getContext().zMStar)
  {
    ctxt.cleanUp();
  }

  shared_ptr<Ctxt> automorph(long i) const override
  {
    shared_ptr<Ctxt> result = make_shared<Ctxt>(ctxt);

    // guard against i == 0, as dim may be #gens
    if (i != 0) result->smartAutomorph(zMStar.genToPow(dim, i));

    return result;
  }
};

class GeneralAutomorphPrecon_FULL : public GeneralAutomorphPrecon {
private:
  BasicAutomorphPrecon precon;
  long dim;
  const PAlgebra& zMStar;

public:
  GeneralAutomorphPrecon_FULL(const Ctxt& _ctxt, long _dim) :
    precon(_ctxt), dim(_dim), zMStar(_ctxt.getContext().zMStar)
  { }

  shared_ptr<Ctxt> automorph(long i) const override
  {
    return precon.automorph(zMStar.genToPow(dim, i));
  }

};

class GeneralAutomorphPrecon_BSGS : public GeneralAutomorphPrecon {
private:
  long dim;
  const PAlgebra& zMStar;

  long D;
  long g;
  long nintervals;
  vector<shared_ptr<BasicAutomorphPrecon>> precon;

public:
  GeneralAutomorphPrecon_BSGS(const Ctxt& _ctxt, long _dim) :
    dim(_dim), zMStar(_ctxt.getContext().zMStar)
  { 
    D = (dim == -1) ? zMStar.getOrdP() : zMStar.OrderOf(dim);
    g = KSGiantStepSize(D);
    nintervals = divc(D, g);

    BasicAutomorphPrecon precon0(_ctxt);
    precon.resize(nintervals);

    // parallel for k in [0..nintervals)
    NTL_EXEC_RANGE(nintervals, first, last)
      for (long k = first; k < last; k++) {
	shared_ptr<Ctxt> p = precon0.automorph(zMStar.genToPow(dim, g*k));
	precon[k] = make_shared<BasicAutomorphPrecon>(*p);
      }
    NTL_EXEC_RANGE_END
  }

  shared_ptr<Ctxt> automorph(long i) const override
  {
    assert(i >= 0 && i < D);
    long j = i % g;
    long k = i / g;
    // i == j + g*k
    return precon[k]->automorph(zMStar.genToPow(dim, j));
  }

};

shared_ptr<GeneralAutomorphPrecon>
buildGeneralAutomorphPrecon(const Ctxt& ctxt, long dim)
{
  // allow dim == -1 (Frobenius)
  // allow dim == #gens (the dummy generator of order 1)
  assert(dim >= -1 && dim <= long(ctxt.getContext().zMStar.numOfGens()));

  switch (ctxt.getPubKey().getKSStrategy(dim)) {
    case FHE_KSS_BSGS:
      return make_shared<GeneralAutomorphPrecon_BSGS>(ctxt, dim);

    case FHE_KSS_FULL:
      return make_shared<GeneralAutomorphPrecon_FULL>(ctxt, dim);
      
    default:
      return make_shared<GeneralAutomorphPrecon_UNKNOWN>(ctxt, dim);
  }
}


/********************************************************************/
/****************** Linear transformation classes *******************/




struct ConstMultiplier {
// stores a constant in either zzX or DoubleCRT format

  virtual ~ConstMultiplier() {}

  virtual void mul(Ctxt& ctxt) const = 0;

  virtual shared_ptr<ConstMultiplier> upgrade(const FHEcontext& context) const = 0;
  // Upgrade to DCRT. Returns null of no upgrade required

};

struct ConstMultiplier_DoubleCRT : ConstMultiplier {

  DoubleCRT data;
  ConstMultiplier_DoubleCRT(const DoubleCRT& _data) : data(_data) { }

  void mul(Ctxt& ctxt) const override {
    ctxt.multByConstant(data);
  } 

  shared_ptr<ConstMultiplier> upgrade(const FHEcontext& context) const override {
    return nullptr;
  }

};


struct ConstMultiplier_zzX : ConstMultiplier {

  zzX data;

  ConstMultiplier_zzX(const zzX& _data) : data(_data) { }

  void mul(Ctxt& ctxt) const override {
    ctxt.multByConstant(data);
  } 

  shared_ptr<ConstMultiplier> upgrade(const FHEcontext& context) const override {
    return make_shared<ConstMultiplier_DoubleCRT>(DoubleCRT(data, context));
  }

};

template<class RX>
shared_ptr<ConstMultiplier> 
build_ConstMultiplier(const RX& poly)
{
   if (IsZero(poly))
      return nullptr;
   else
      return make_shared<ConstMultiplier_zzX>(convert<zzX>(poly));
}

template<class RX, class type>
shared_ptr<ConstMultiplier> 
build_ConstMultiplier(const RX& poly, 
                      long dim, long amt, const EncryptedArrayDerived<type>& ea)
{
   if (IsZero(poly))
      return nullptr;
   else {
      RX poly1;
      plaintextAutomorph(poly1, poly, dim, amt, ea);
      return make_shared<ConstMultiplier_zzX>(convert<zzX>(poly1));
   }
}


void MulAdd(Ctxt& x, const shared_ptr<ConstMultiplier>& a, const Ctxt& b)
// x += a*b
{
   if (a) {
      Ctxt tmp(b);
      a->mul(tmp);
      x += tmp;
   }
}

void DestMulAdd(Ctxt& x, const shared_ptr<ConstMultiplier>& a, Ctxt& b)
// x += a*b, b may be modified
{
   if (a) {
      a->mul(b);
      x += b;
   }
}


void ConstMultiplierCache::upgrade(const FHEcontext& context) 
{
  FHE_TIMER_START;

  long n = multiplier.size();
  NTL_EXEC_RANGE(n, first, last)
  for (long i: range(first, last)) {
    if (multiplier[i]) 
      if (auto newptr = multiplier[i]->upgrade(context)) 
	multiplier[i] = shared_ptr<ConstMultiplier>(newptr); 
  }
  NTL_EXEC_RANGE_END
}



static inline long dimSz(const EncryptedArray& ea, long dim)
{
   return (dim==ea.dimension())? 1 : ea.sizeOfDimension(dim);
}

static inline long dimSz(const EncryptedArrayBase& ea, long dim)
{
   return (dim==ea.dimension())? 1 : ea.sizeOfDimension(dim);
}

static inline long dimNative(const EncryptedArray& ea, long dim)
{
   return (dim==ea.dimension())? true : ea.nativeDimension(dim);
}

static inline long dimNative(const EncryptedArrayBase& ea, long dim)
{
   return (dim==ea.dimension())? true : ea.nativeDimension(dim);
}


template<class type>
struct MatMul1DExec_construct {
  PA_INJECT(type)

  static
  void processDiagonal1(RX& poly, long i, long rotAmt,
                        const EncryptedArrayDerived<type>& ea,
                        const MatMul1D_derived<type>& mat)
  {
    long dim = mat.getDim();
    long D = dimSz(ea, dim);

    vector<RX> tmpDiag(D);
    bool zDiag = true; // is this a zero diagonal?
    long nzLast = -1;  // index of last non-zero entry
    RX entry;

    // Process the entries in this diagonal one at a time
    for (long j = 0; j < D; j++) { // process entry j
      long rotJ = (j+rotAmt) % D;  // need to rotate constant by rotAmt
      bool zEntry = mat.get(entry, mcMod(rotJ-i, D), rotJ, 0); 
        // entry [j-i mod D, j]

      assert(zEntry || deg(entry) < ea.getDegree());
      // get(...) returns true if the entry is empty, false otherwise

      if (!zEntry && IsZero(entry)) zEntry = true;// zero is an empty entry too

      if (!zEntry) {   // not a zero entry
        zDiag = false; // mark diagonal as non-empty

        // clear entries between last nonzero entry and this one
        for (long jj = nzLast+1; jj < j; jj++) clear(tmpDiag[jj]);
        tmpDiag[j] = entry;
        nzLast = j;
      }
    }    
    if (zDiag) {
      clear(poly);
    } 
    else {

      // clear trailing zero entries
      for (long jj = nzLast+1; jj < D; jj++) clear(tmpDiag[jj]);
      
      vector<RX> diag(ea.size());
      if (D==1) 
	diag.assign(ea.size(), tmpDiag[0]); // dimension of size one
      else {
	for (long j = 0; j < ea.size(); j++)
	  diag[j] = tmpDiag[ ea.coordinate(dim,j) ];
	  // rearrange the indexes based on the current dimension
      }

      ea.encode(poly, diag);
    }
  }


  static
  void processDiagonal2(RX& poly, long idx, long rotAmt,
                        const EncryptedArrayDerived<type>& ea,
                        const MatMul1D_derived<type>& mat)
  {
    long dim = mat.getDim();
    long D = dimSz(ea, dim);

    bool zDiag = true; // is this a zero diagonal?
    long nzLast = -1;  // index of last non-zero entry
    RX entry;

    long n = ea.size();

    // Process the entries in this diagonal one at a time
    long blockIdx, innerIdx;
    vector<RX> diag(n);
    for (long j=0; j < n; j++) {
      if (D==1) {
	blockIdx=j; 
        innerIdx = 0;
      } 
      else {
	std::tie(blockIdx, innerIdx) // std::pair<long,long> idxes
	  = ea.getContext().zMStar.breakIndexByDim(j, dim);
	//	blockIdx = idxes.first;  // which transformation
	//	innerIdx = idxes.second; // index along dimension dim
        innerIdx = (innerIdx+rotAmt) % D;  // need to rotate constant by rotAmt
      }
      // process entry j
      bool zEntry=mat.get(entry, mcMod(innerIdx-idx,D), innerIdx, blockIdx);
      // entry [i,j-i mod D] in the block corresponding to blockIdx
      // get(...) returns true if the entry is empty, false otherwise

      // If non-zero, make sure the degree is not too large
      assert(zEntry || deg(entry) < ea.getDegree());

      if (!zEntry && IsZero(entry)) zEntry = true; // zero is an empty entry too

      if (!zEntry) {   // not a zero entry
	zDiag = false; // mark diagonal as non-empty

	// clear entries between last nonzero entry and this one
	for (long jj = nzLast+1; jj < j; jj++) clear(diag[jj]);
	nzLast = j;
	diag[j] = entry;
      }
    }    
    if (zDiag) {
      clear(poly);
    }
    else {

      // clear trailing zero entries
      for (long jj = nzLast+1; jj < ea.size(); jj++) clear(diag[jj]);

      ea.encode(poly, diag);
    }
  }

  // Get the i'th diagonal, encoded as a single constant. 
  static
  void processDiagonal(RX& poly, long i, long rotAmt,
                        const EncryptedArrayDerived<type>& ea,
                        const MatMul1D_derived<type>& mat)
  {
    if (mat.multipleTransforms())
      processDiagonal2(poly, i, rotAmt, ea, mat);
    else
      processDiagonal1(poly, i, rotAmt, ea, mat);
  }

  static
  void apply(const EncryptedArrayDerived<type>& ea,
             const MatMul1D& mat_basetype,
             vector<shared_ptr<ConstMultiplier>>& vec,
             vector<shared_ptr<ConstMultiplier>>& vec1,
             long g)
  {
    const MatMul1D_derived<type>& mat =
      dynamic_cast< const MatMul1D_derived<type>& >(mat_basetype);

    long dim = mat.getDim();
    long D = dimSz(ea, dim);
    bool native = dimNative(ea, dim);

    RBak bak; bak.save(); ea.getTab().restoreContext();

    if (native) {

      vec.resize(D);

      for (long i: range(D)) {
	// i == j + g*k
        long j, k;
      
        if (g) {
          j = i % g;
          k = i / g;
        }
        else {
          j = i;
          k = 1;
        }

	RX poly;
	processDiagonal(poly, i, 0, ea, mat);
        vec[i] = build_ConstMultiplier(poly, dim, -g*k, ea);
      }
    }
    else {
      vec.resize(D);
      vec1.resize(D);

      for (long i: range(D)) {
	// i == j + g*k
        long j, k;
      
        if (g) {
          j = i % g;
          k = i / g;
        }
        else {
          j = i;
          k = 1;
        }

	RX poly;
	processDiagonal(poly, i, 0, ea, mat);

        if (IsZero(poly)) {
          vec[i] = nullptr;
          vec1[i] = nullptr;
          continue;
        }

        const RX& mask = ea.getTab().getMaskTable()[dim][i];
        const RXModulus& PhimXMod = ea.getTab().getPhimXMod();

        RX poly1, poly2;
        MulMod(poly1, poly, mask, PhimXMod);
        sub(poly2, poly, poly1);

        // poly1 = poly w/ first i slots zeroed out
        // poly2 = poly w/ last D-i slots zeroed out

        vec[i] = build_ConstMultiplier(poly1, dim, -g*k, ea);
        vec1[i] = build_ConstMultiplier(poly2, dim, D-g*k, ea);
      }
    }
  }
};


#define FHE_BSGS_MUL_THRESH FHE_KEYSWITCH_THRESH
// uses a BSGS multiplication strategy if sizeof(dim) > FHE_BSGS_MUL_THRESH;
// otherwise uses the old strategy (but potentially with hoisting)

// For performace purposes, should not exceed FHE_KEYSWITCH_THRESH
// For testing purposes: 
//    set to 1 to always use BSGS
//    set to infty to never use BSGS            



MatMul1DExec::MatMul1DExec(const MatMul1D& mat, bool _minimal)
  : ea(mat.getEA()), minimal(_minimal)
{
    FHE_NTIMER_START(MatMul1DExec);

    dim = mat.getDim();
    assert(dim >= 0 && dim <= ea.dimension());
    D = dimSz(ea, dim);
    native = dimNative(ea, dim);

    // FIXME: performance tune
    if (D <= FHE_BSGS_MUL_THRESH || minimal)
       g = 0; // do not use BSGS
    else
       g = KSGiantStepSize(D); // use BSGS

    ea.dispatch<MatMul1DExec_construct>(mat, Fwd(cache.multiplier), 
                                        Fwd(cache1.multiplier), g);
}


/***************************************************************************

BS/GS logic:

  \sum_{i=0}^{D-1} const_i rot^i(v)
    = \sum_k \sum_j const_{j+g*k} rot^{j+g*k}(v)
    = \sum_k rot^{g*k}[ \sum_j rot^{-g*k}(const_{j+g*k}) rot^j(v) ]

So we first compute baby_steps[j] = rot^j(v) for j in [0..g).
Then for each k in [0..ceil(D/g)), we compute 
   giant_steps[k] = \rot^{g*k}[ rot^{-g*k}(const_{j+g*k}) baby_steps[j] ] 
Then we add up all the giant_steps.

In bad dimesnions:

We need to compute
\[
  \sum_{j,k} c_{j+gk} r^{j+gk}(x)
\]
where $r^i$ denotes rotation by $i$.
In bad dimensions, we have
\[
 r^i(x) = d_i \rho^i(x) + e_i \rho^{i-D}(x)
\]
for constants $d_i$ and $e_i$.
Here, d_i is maskTable[i][amt] and e_i = 1-d_i

So putting it all together
\[
  \sum_{j,k} c_{j+gk} r^{j+gk}(x)
= \sum_{j,k} d'_{j+gk} \rho^{j+gk}(x) + e'_{j+gk} \rho^{j+gk-D}(x) 
     \text{where $d'_i=c_i d_i$ and $e'_i = c_i e_i$}


=               \sum_k \rho^{gk}[ \sum_j d''_{j+gk} \rho^j(x) ]
   + \rho^{-D}[ \sum_k \rho^{gk}[ \sum_j e''_{j+gk} \rho^j(x) ] ]
      \text{where $d''_{j+gk} = \rho^{-gk}(d'_{j+gk})$ and
                  $e''_{j+gk} = \rho^{D-gk}(d'_{j+gk})$}
 
\]

***************************************************************************/

void GenBabySteps(vector<shared_ptr<Ctxt>>& v, const Ctxt& ctxt, long dim, 
                  bool clean)
{
  long n = v.size();
  assert(n > 0);

  if (n == 1) {
    v[0] = make_shared<Ctxt>(ctxt);
    if (clean) v[0]->cleanUp();
    return;
  }

  const PAlgebra& zMStar = ctxt.getContext().zMStar;

  if (ctxt.getPubKey().getKSStrategy(dim) != FHE_KSS_UNKNOWN) {
    BasicAutomorphPrecon precon(ctxt);

    NTL_EXEC_RANGE(n, first, last)
      for (long j: range(first, last)) {
	 v[j] = precon.automorph(zMStar.genToPow(dim, j));
	 if (clean) v[j]->cleanUp();
      }
    NTL_EXEC_RANGE_END
  }
  else {
    Ctxt ctxt0(ctxt);
    ctxt0.cleanUp();
 
    NTL_EXEC_RANGE(n, first, last)
      for (long j: range(first, last)) {
	 v[j] = make_shared<Ctxt>(ctxt0);
	 v[j]->smartAutomorph(zMStar.genToPow(dim, j));
	 if (clean) v[j]->cleanUp();
      }
    NTL_EXEC_RANGE_END
  }
  
}

void
MatMul1DExec::mul(Ctxt& ctxt) const
{
   assert(&ea.getContext() == &ctxt.getContext());
   const PAlgebra& zMStar = ea.getContext().zMStar;

   ctxt.cleanUp();

   if (g != 0) {
      // baby-step / giant-step

      if (native) {
	 long nintervals = divc(D, g);
	 vector<shared_ptr<Ctxt>> baby_steps(g);
	 GenBabySteps(baby_steps, ctxt, dim, true);

	 PartitionInfo pinfo(nintervals);
	 long cnt = pinfo.NumIntervals();

	 vector<Ctxt> acc(cnt, Ctxt(ZeroCtxtLike, ctxt));

	 // parallel for loop: k in [0..nintervals)
	 NTL_EXEC_INDEX(cnt, index)
	    long first, last;
	    pinfo.interval(first, last, index);

	    for (long k: range(first, last)) {
	       Ctxt acc_inner(ZeroCtxtLike, ctxt);

	       for (long j: range(g)) {
		  long i = j + g*k;
		  if (i >= D) break;
                  MulAdd(acc_inner, cache.multiplier[i], *baby_steps[j]); 
	       }

	       if (k > 0) acc_inner.smartAutomorph(zMStar.genToPow(dim, g*k));
	       acc[index] += acc_inner;
	    }
	 NTL_EXEC_INDEX_END

	 ctxt = acc[0];
	 for (long i: range(1, cnt))
	    ctxt += acc[i];
      }
      else {
	 long nintervals = divc(D, g);
	 vector<shared_ptr<Ctxt>> baby_steps(g);
	 GenBabySteps(baby_steps, ctxt, dim, true);

	 PartitionInfo pinfo(nintervals);
	 long cnt = pinfo.NumIntervals();

	 vector<Ctxt> acc(cnt, Ctxt(ZeroCtxtLike, ctxt));
	 vector<Ctxt> acc1(cnt, Ctxt(ZeroCtxtLike, ctxt));

	 // parallel for loop: k in [0..nintervals)
	 NTL_EXEC_INDEX(cnt, index)

	    long first, last;
	    pinfo.interval(first, last, index);

	    for (long k: range(first, last)) {
	       Ctxt acc_inner(ZeroCtxtLike, ctxt);
	       Ctxt acc_inner1(ZeroCtxtLike, ctxt);

	       for (long j: range(g)) {
		  long i = j + g*k;
		  if (i >= D) break;
                  MulAdd(acc_inner, cache.multiplier[i], *baby_steps[j]);
                  MulAdd(acc_inner1, cache1.multiplier[i], *baby_steps[j]);
	       }

	       if (k > 0) {
		  acc_inner.smartAutomorph(zMStar.genToPow(dim, g*k));
		  acc_inner1.smartAutomorph(zMStar.genToPow(dim, g*k));
	       }

	       acc[index] += acc_inner;
	       acc1[index] += acc_inner1;
	    }

	 NTL_EXEC_INDEX_END

	 for (long i: range(1, cnt)) acc[0] += acc[i];
	 for (long i: range(1, cnt)) acc1[0] += acc1[i];

	 acc1[0].smartAutomorph(zMStar.genToPow(dim, -D));
	 acc[0] += acc1[0];
	 ctxt = acc[0];
      }
   }
   else if (!minimal) {
      if (native) {
         shared_ptr<GeneralAutomorphPrecon> precon =
            buildGeneralAutomorphPrecon(ctxt, dim);

	 PartitionInfo pinfo(D);
	 long cnt = pinfo.NumIntervals();

	 vector<Ctxt> acc(cnt, Ctxt(ZeroCtxtLike, ctxt));

	 // parallel for loop: i in [0..D)
	 NTL_EXEC_INDEX(cnt, index)
	    long first, last;
	    pinfo.interval(first, last, index);

	    for (long i: range(first, last)) {
	       if (cache.multiplier[i]) {
		  shared_ptr<Ctxt> tmp = precon->automorph(i);
                  DestMulAdd(acc[index], cache.multiplier[i], *tmp);
	       }
	    }
	 NTL_EXEC_INDEX_END

	 ctxt = acc[0];
	 for (long i: range(1, cnt))
	    ctxt += acc[i];
      }
      else {
         shared_ptr<GeneralAutomorphPrecon> precon =
            buildGeneralAutomorphPrecon(ctxt, dim);

	 PartitionInfo pinfo(D);
	 long cnt = pinfo.NumIntervals();

	 vector<Ctxt> acc(cnt, Ctxt(ZeroCtxtLike, ctxt));
	 vector<Ctxt> acc1(cnt, Ctxt(ZeroCtxtLike, ctxt));

	 // parallel for loop: i in [0..D)
	 NTL_EXEC_INDEX(cnt, index)
	    long first, last;
	    pinfo.interval(first, last, index);

	    for (long i: range(first, last)) {
	       if (cache.multiplier[i] || cache1.multiplier[i]) {
		  shared_ptr<Ctxt> tmp = precon->automorph(i);
                  MulAdd(acc[index], cache.multiplier[i], *tmp);
                  DestMulAdd(acc1[index], cache1.multiplier[i], *tmp);
	       }
	    }
	 NTL_EXEC_INDEX_END

	 for (long i: range(1, cnt)) acc[0] += acc[i];
	 for (long i: range(1, cnt)) acc1[0] += acc1[i];

	 acc1[0].smartAutomorph(zMStar.genToPow(dim, -D));
	 acc[0] += acc1[0];
	 ctxt = acc[0];
      }
   }
   else /* minimal */ {
      if (native) {
	 Ctxt acc(ZeroCtxtLike, ctxt);
         Ctxt sh_ctxt(ctxt);
 
         for (long i: range(D)) {
	    if (i > 0) sh_ctxt.smartAutomorph(zMStar.genToPow(dim, 1));
            MulAdd(acc, cache.multiplier[i], sh_ctxt);
         }

	 ctxt = acc;
      }
      else {
	 Ctxt acc(ZeroCtxtLike, ctxt);
	 Ctxt acc1(ZeroCtxtLike, ctxt);
         Ctxt sh_ctxt(ctxt);
 
         for (long i: range(D)) {
	    if (i > 0) sh_ctxt.smartAutomorph(zMStar.genToPow(dim, 1));
            MulAdd(acc, cache.multiplier[i], sh_ctxt);
            MulAdd(acc1, cache1.multiplier[i], sh_ctxt);
         }

	 acc1.smartAutomorph(zMStar.genToPow(dim, -D));
	 acc += acc1;
	 ctxt = acc;
      }
   }
}



// ========================== BlockMatMul1D stuff =====================

template<class type>
struct BlockMatMul1DExec_construct {
  PA_INJECT(type)

  // return true if zero
  static
  bool processDiagonal1(vector<RX>& poly, long i, 
                        const EncryptedArrayDerived<type>& ea,
                        const BlockMatMul1D_derived<type>& mat)
  {
    long dim = mat.getDim();
    long D = dimSz(ea, dim);
    long nslots = ea.size();
    long d = ea.getDegree();

    bool zDiag = true; // is this a zero diagonal?
    long nzLast = -1;  // index of last non-zero entry

    mat_R entry(INIT_SIZE, d, d);
    std::vector<RX> entry1(d);
    std::vector< std::vector<RX> > tmpDiag(D);

    vector<vector<RX>> diag(nslots);

    // Process the entries in this diagonal one at a time
    for (long j: range(D)) { // process entry j
      bool zEntry = mat.get(entry, mcMod(j-i, D), j, 0); // entry [j-i mod D, j]
      // get(...) returns true if the entry is empty, false otherwise

      if (!zEntry && IsZero(entry)) zEntry = true;// zero is an empty entry too
      assert(zEntry || (entry.NumRows() == d && entry.NumCols() == d));

      if (!zEntry) {   // not a zero entry
        zDiag = false; // mark diagonal as non-empty

	for (long jj: range(nzLast+1, j)) {// clear from last nonzero entry
          tmpDiag[jj].assign(d, RX());
        }
        nzLast = j; // current entry is the last nonzero one

        // recode entry as a vector of polynomials
        for (long k: range(d)) conv(entry1[k], entry[k]);

        // compute the linearlized polynomial coefficients
	ea.buildLinPolyCoeffs(tmpDiag[j], entry1);
      }
    }
    if (zDiag) return true; // zero diagonal, nothing to do

    // clear trailing zero entries
    for (long jj: range(nzLast+1, D)) {
      tmpDiag[jj].assign(d, RX());
    }

    if (D==1) 
       diag.assign(nslots, tmpDiag[0]); // dimension of size one
    else {
      for (long j: range(nslots))
        diag[j] = tmpDiag[ ea.coordinate(dim,j) ];
           // rearrange the indexes based on the current dimension
    }

    // transpose and encode diag to form polys

    vector<RX> slots(nslots);
    poly.resize(d);
    for (long i: range(d)) {
      for (long j: range(nslots)) slots[j] = diag[j][i];
      ea.encode(poly[i], slots);
    }

    return false; // a nonzero diagonal


  }

  // return true if zero
  static
  bool processDiagonal2(vector<RX>& poly, long idx,
                        const EncryptedArrayDerived<type>& ea,
                        const BlockMatMul1D_derived<type>& mat)
  {
    long dim = mat.getDim();
    long D = dimSz(ea, dim);
    long nslots = ea.size();
    long d = ea.getDegree();

    bool zDiag = true; // is this a zero diagonal?
    long nzLast = -1;  // index of last non-zero entry

    mat_R entry(INIT_SIZE, d, d);
    std::vector<RX> entry1(d);

    vector<vector<RX>> diag(nslots);

    // Get the slots in this diagonal one at a time
    long blockIdx, rowIdx, colIdx;
    for (long j: range(nslots)) { // process entry j
      if (dim == ea.dimension()) { // "special" last dimenssion of size 1
	rowIdx = colIdx = 0; blockIdx=j;
      } 
      else {
        std::tie(blockIdx, colIdx)
	  = ea.getContext().zMStar.breakIndexByDim(j, dim);
	rowIdx = mcMod(colIdx-idx,D);
      }
      bool zEntry = mat.get(entry,rowIdx,colIdx,blockIdx);
      // entry [i,j-i mod D] in the block corresponding to blockIdx
      // get(...) returns true if the entry is empty, false otherwise

      if (!zEntry && IsZero(entry)) zEntry=true; // zero is an empty entry too
      assert(zEntry ||
             (entry.NumRows() == d && entry.NumCols() == d));

      if (!zEntry) {    // non-empty entry
	zDiag = false;  // mark diagonal as non-empty

	for (long jj: range(nzLast+1, j)) // clear from last nonzero entry
          diag[jj].assign(d, RX());

	nzLast = j; // current entry is the last nonzero one

	// recode entry as a vector of polynomials
	for (long k: range(d)) conv(entry1[k], entry[k]);

        // compute the linearlized polynomial coefficients
	ea.buildLinPolyCoeffs(diag[j], entry1);
      }
    }
    if (zDiag) return true; // zero diagonal, nothing to do

    // clear trailing zero entries
    for (long jj: range(nzLast+1, nslots))
      diag[jj].assign(d, RX());

    // transpose and encode diag to form polys

    vector<RX> slots(nslots);
    poly.resize(d);
    for (long i: range(d)) {
      for (long j: range(nslots)) slots[j] = diag[j][i];
      ea.encode(poly[i], slots);
    }

    return false; // a nonzero diagonal
  }

  // return true if zero
  static
  bool processDiagonal(vector<RX>& poly, long i,
                        const EncryptedArrayDerived<type>& ea,
                        const BlockMatMul1D_derived<type>& mat)
  {
    if (mat.multipleTransforms())
      return processDiagonal2(poly, i, ea, mat);
    else
      return processDiagonal1(poly, i,  ea, mat);

  }


  // Basic logic:
  // We are computing \sum_i \sum_j c_{ij} \sigma^j rot^i(v),
  // where \sigma = frobenius, rot = rotation 
  // For good dimensions, rot = \rho (the basic automorphism),
  // so we need to compute
  //     \sum_i \sum_j c_{ij} \sigma^j \rho^i(v)
  //  =  \sum_j \sigma^j[ \sigma^{-j}(c_{ij}) \rho^i(v) ]

  // For bad dimensions, we have 
  //   rot^i(v) = d_i \rho^i(v) + e_i \rho^{i-D}(v)
  // and so we need to compute
  //   \sum_i \sum_j c_{ij} \sigma^j (d_i \rho^i(v) + e_i \rho^{i-D}(v))
  // =      \sum_j \sigma_j[  \sigma^{-j}(c_{ij}) d_i \rho^i(v) ] +
  //   \rho^{-D}[ \sum_j \sigma_j[ \rho^{D}{\sigma^{-j}(c_{ij}) e_i} \rho^i(v) ] ]


  // strategy == +1 : factor \sigma
  // strategy == -1 : factor \rho
  // strategy ==  0 : no factoring (used to implemengt minimal KS strategy)

   

  static
  void apply(const EncryptedArrayDerived<type>& ea,
             const BlockMatMul1D& mat_basetype,
             vector<shared_ptr<ConstMultiplier>>& vec,
             vector<shared_ptr<ConstMultiplier>>& vec1,
             long strategy)
  {
    const BlockMatMul1D_derived<type>& mat =
      dynamic_cast< const BlockMatMul1D_derived<type>& >(mat_basetype);

    long dim = mat.getDim();
    long D = dimSz(ea, dim);
    long d = ea.getDegree();
    bool native = dimNative(ea, dim);

    RBak bak; bak.save(); ea.getTab().restoreContext();

    vector<RX> poly;

    switch (strategy) {
    case +1: // factor \sigma

      if (native) {
        vec.resize(D*d);
        for (long i: range(D)) {
          bool zero = processDiagonal(poly, i, ea, mat);
          if (zero) {
            for (long j: range(d)) vec[i*d+j] = nullptr;
          }
          else {
	    for (long j: range(d)) {
	      vec[i*d+j] = build_ConstMultiplier(poly[j], -1, -j, ea);
	    }
          }
        }
      }
      else {
        vec.resize(D*d);
        vec1.resize(D*d);
        for (long i: range(D)) {
          bool zero = processDiagonal(poly, i, ea, mat);
          if (zero) {
            for (long j: range(d)) {
              vec [i*d+j] = nullptr;
              vec1[i*d+j] = nullptr;
            }
          }
          else {
	    const RX& mask = ea.getTab().getMaskTable()[dim][i];
	    const RXModulus& F = ea.getTab().getPhimXMod();

            for (long j: range(d)) {
              plaintextAutomorph(poly[j], poly[j], -1, -j, ea);

              RX poly1;
              MulMod(poly1, poly[j], mask, F); // poly[j] w/ first i slots zeroed out
              vec[i*d+j] = build_ConstMultiplier(poly1);

              sub(poly1, poly[j], poly1); // poly[j] w/ last D-i slots zeroed out
              vec1[i*d+j] = build_ConstMultiplier(poly1, dim, D, ea);
            }
          }
        }
      }
      break;

    case -1: // factor \rho

      if (native) {
        vec.resize(D*d);
        for (long i: range(D)) {
          bool zero = processDiagonal(poly, i, ea, mat);
          if (zero) {
            for (long j: range(d)) vec[i+j*D] = nullptr;
          }
          else {
	    for (long j: range(d)) {
	      vec[i+j*D] = build_ConstMultiplier(poly[j], dim, -i, ea);
	    }
          }
        }
      }
      else {
        vec.resize(D*d);
        vec1.resize(D*d);
        for (long i: range(D)) {
          bool zero = processDiagonal(poly, i, ea, mat);
          if (zero) {
            for (long j: range(d)) {
              vec [i+j*D] = nullptr;
              vec1[i+j*D] = nullptr;
            }
          }
          else {
	    const RX& mask = ea.getTab().getMaskTable()[dim][i];
	    const RXModulus& F = ea.getTab().getPhimXMod();

            for (long j: range(d)) {
              RX poly1, poly2;
              MulMod(poly1, poly[j], mask, F); // poly[j] w/ first i slots zeroed out
              sub(poly2, poly[j], poly1);      // poly[j] w/ last D-i slots zeroed out

              vec[i+j*D] = build_ConstMultiplier(poly1, dim, -i, ea);
              vec1[i+j*D] = build_ConstMultiplier(poly2, dim, D-i, ea);
            }
          }
        }
      }

      break;

    case 0: // no factoring

      if (native) {
        vec.resize(D*d);
        for (long i: range(D)) {
          bool zero = processDiagonal(poly, i, ea, mat);
          if (zero) {
            for (long j: range(d)) vec[i*d+j] = nullptr;
          }
          else {
	    for (long j: range(d)) {
	      vec[i*d+j] = build_ConstMultiplier(poly[j]);
	    }
          }
        }
      }
      else {
        vec.resize(D*d);
        vec1.resize(D*d);
        for (long i: range(D)) {
          bool zero = processDiagonal(poly, i, ea, mat);
          if (zero) {
            for (long j: range(d)) {
              vec [i*d+j] = nullptr;
              vec1[i*d+j] = nullptr;
            }
          }
          else {
	    const RX& mask = ea.getTab().getMaskTable()[dim][i];
	    const RXModulus& F = ea.getTab().getPhimXMod();

            for (long j: range(d)) {
              RX poly1, poly2;
              MulMod(poly1, poly[j], mask, F); // poly[j] w/ first i slots zeroed out
              sub(poly2, poly[j], poly1);      // poly[j] w/ last D-i slots zeroed out

              vec[i*d+j] = build_ConstMultiplier(poly1);
              vec1[i*d+j] = build_ConstMultiplier(poly2, dim, D, ea);
            }
          }
        }
      }

      break;

    default:
      Error("unknown strategy");
    }
      
  }

};


BlockMatMul1DExec::BlockMatMul1DExec(const BlockMatMul1D& mat, bool minimal)
  : ea(mat.getEA())
{
    FHE_TIMER_START;

    dim = mat.getDim();
    assert(dim >= 0 && dim <= ea.dimension());
    D = dimSz(ea, dim);
    d = ea.getDegree();
    native = dimNative(ea, dim);
    
    if (minimal) 
      strategy = 0;
    else if (D >= d) 
      strategy = +1;
    else
      strategy = -1;

    ea.dispatch<BlockMatMul1DExec_construct>(mat, Fwd(cache.multiplier), 
                                        Fwd(cache1.multiplier), strategy);
}


void
BlockMatMul1DExec::mul(Ctxt& ctxt) const
{
   assert(&ea.getContext() == &ctxt.getContext());
   const PAlgebra& zMStar = ea.getContext().zMStar;

   ctxt.cleanUp();

   if (strategy == 0) {
      // assumes minimal KS matrices present

      if (native) {
	 Ctxt acc(ZeroCtxtLike, ctxt);
         Ctxt sh_ctxt(ctxt);
 
         for (long i: range(D)) {
	    if (i > 0) sh_ctxt.smartAutomorph(zMStar.genToPow(dim, 1));
            Ctxt sh_ctxt1(sh_ctxt);

            for (long j: range(d)) {
	       if (j > 0) sh_ctxt1.smartAutomorph(zMStar.genToPow(-1, 1));
               MulAdd(acc, cache.multiplier[i*d+j], sh_ctxt1);
            }
         }

	 ctxt = acc;
      }
      else {
	 Ctxt acc(ZeroCtxtLike, ctxt);
	 Ctxt acc1(ZeroCtxtLike, ctxt);
         Ctxt sh_ctxt(ctxt);
 
         for (long i: range(D)) {
	    if (i > 0) sh_ctxt.smartAutomorph(zMStar.genToPow(dim, 1));
            Ctxt sh_ctxt1(sh_ctxt);

            for (long j: range(d)) {
	       if (j > 0) sh_ctxt1.smartAutomorph(zMStar.genToPow(-1, 1));
               MulAdd(acc, cache.multiplier[i*d+j], sh_ctxt1);
               MulAdd(acc1, cache1.multiplier[i*d+j], sh_ctxt1);
            }
         }

	 acc1.smartAutomorph(zMStar.genToPow(dim, -D));
	 acc += acc1;
	 ctxt = acc;
      }

      return;
   }

   long d0, d1;
   long dim0, dim1;

   if (strategy == +1) {
      d0 = D;
      dim0 = dim;
      d1 = d;
      dim1 = -1;
   }
   else {
      d1 = D;
      dim1 = dim;
      d0 = d;
      dim0 = -1;
   }

   const long par_buf_max = 50;

   if (native) {
      vector<Ctxt> acc(d1, Ctxt(ZeroCtxtLike, ctxt));

      shared_ptr<GeneralAutomorphPrecon> precon =
	       buildGeneralAutomorphPrecon(ctxt, dim0);

#if 0
      // This is the original code

      for (long i: range(d0)) {
	 shared_ptr<Ctxt> tmp = precon->automorph(i);
	 for (long j: range(d1)) {
            MulAdd(acc[j], cache.multiplier[i*d1+j], *tmp);
	 }
      }

      Ctxt sum(ZeroCtxtLike, ctxt);
      for (long j: range(d1)) {
	 if (j > 0) acc[j].smartAutomorph(zMStar.genToPow(dim1, j));
	 sum += acc[j];
      }

      ctxt = sum;
#endif

      long par_buf_sz = 1;
      if (AvailableThreads() > 1) 
         par_buf_sz = min(d0, par_buf_max);

      vector<shared_ptr<Ctxt>> par_buf(par_buf_sz);

      for (long first_i = 0; first_i < d0; first_i += par_buf_sz) {
         long last_i = min(first_i + par_buf_sz, d0);

         // for i in [first_i..last_i), generate automorphosm i and store
         // in par_buf[i-first_i]

         NTL_EXEC_RANGE(last_i-first_i, first, last) 
  
            for (long idx: range(first, last)) {
              long i = idx + first_i;
              par_buf[idx] = precon->automorph(i);
            }

         NTL_EXEC_RANGE_END

         NTL_EXEC_RANGE(d1, first, last)

            for (long j: range(first, last)) {
               for (long i: range(first_i, last_i)) {
                  MulAdd(acc[j], cache.multiplier[i*d1+j], *par_buf[i-first_i]);
               }
            }

         NTL_EXEC_RANGE_END
      }

      par_buf.resize(0); // free-up space

      PartitionInfo pinfo(d1);
      long cnt = pinfo.NumIntervals();

      vector<Ctxt> sum(cnt, Ctxt(ZeroCtxtLike, ctxt));

      // for j in [0..d1)
      NTL_EXEC_INDEX(cnt, index)
         long first, last;
         pinfo.interval(first, last, index);
         for (long j: range(first, last)) {
	    if (j > 0) acc[j].smartAutomorph(zMStar.genToPow(dim1, j));
	    sum[index] += acc[j];
         }
      NTL_EXEC_INDEX_END

      ctxt = sum[0];
      for (long i: range(1, cnt)) ctxt += sum[i];
   }
   else {

      vector<Ctxt> acc(d1, Ctxt(ZeroCtxtLike, ctxt));
      vector<Ctxt> acc1(d1, Ctxt(ZeroCtxtLike, ctxt));

      shared_ptr<GeneralAutomorphPrecon> precon =
	       buildGeneralAutomorphPrecon(ctxt, dim0);

#if 0
      // original code


      for (long i: range(d0)) {
	 shared_ptr<Ctxt> tmp = precon->automorph(i);
         MulAdd(acc[j], cache.multiplier[i*d1+j], *tmp);
         MulAdd(acc1[j], cache1.multiplier[i*d1+j], *tmp);
      }

      Ctxt sum(ZeroCtxtLike, ctxt);
      Ctxt sum1(ZeroCtxtLike, ctxt);
      for (long j: range(d1)) {
	 if (j > 0) {
            acc[j].smartAutomorph(zMStar.genToPow(dim1, j));
            acc1[j].smartAutomorph(zMStar.genToPow(dim1, j));
         }
	 sum += acc[j];
	 sum1 += acc1[j];
      }

      sum1.smartAutomorph(zMStar.genToPow(dim, -D));
      sum += sum1;
      ctxt = sum;
#endif

      long par_buf_sz = 1;
      if (AvailableThreads() > 1) 
         par_buf_sz = min(d0, par_buf_max);

      vector<shared_ptr<Ctxt>> par_buf(par_buf_sz);

      for (long first_i = 0; first_i < d0; first_i += par_buf_sz) {
         long last_i = min(first_i + par_buf_sz, d0);

         // for i in [first_i..last_i), generate automorphosm i and store
         // in par_buf[i-first_i]

         NTL_EXEC_RANGE(last_i-first_i, first, last) 
  
            for (long idx: range(first, last)) {
              long i = idx + first_i;
              par_buf[idx] = precon->automorph(i);
            }

         NTL_EXEC_RANGE_END

         NTL_EXEC_RANGE(d1, first, last)

            for (long j: range(first, last)) {
               for (long i: range(first_i, last_i)) {
                  MulAdd(acc[j], cache.multiplier[i*d1+j], *par_buf[i-first_i]);
                  MulAdd(acc1[j], cache1.multiplier[i*d1+j], *par_buf[i-first_i]);
               }
            }

         NTL_EXEC_RANGE_END
      }

      par_buf.resize(0); // free-up space

      PartitionInfo pinfo(d1);
      long cnt = pinfo.NumIntervals();

      vector<Ctxt> sum(cnt, Ctxt(ZeroCtxtLike, ctxt));
      vector<Ctxt> sum1(cnt, Ctxt(ZeroCtxtLike, ctxt));

      // for j in [0..d1)
      NTL_EXEC_INDEX(cnt, index)
         long first, last;
         pinfo.interval(first, last, index);
         for (long j: range(first, last)) {
	    if (j > 0) {
               acc[j].smartAutomorph(zMStar.genToPow(dim1, j));
               acc1[j].smartAutomorph(zMStar.genToPow(dim1, j));
            }
	    sum[index] += acc[j];
	    sum1[index] += acc1[j];
         }
      NTL_EXEC_INDEX_END

      for (long i: range(1, cnt)) sum[0] += sum[i];
      for (long i: range(1, cnt)) sum1[0] += sum1[i];
      sum1[0].smartAutomorph(zMStar.genToPow(dim, -D));
      ctxt = sum[0];
      ctxt += sum1[0];
   }
}
