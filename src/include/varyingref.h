/*
  Copyright 2008 Larry Gritz and the other authors and contributors.
  All Rights Reserved.

  Redistribution and use in source and binary forms, with or without
  modification, are permitted provided that the following conditions are
  met:
  * Redistributions of source code must retain the above copyright
    notice, this list of conditions and the following disclaimer.
  * Redistributions in binary form must reproduce the above copyright
    notice, this list of conditions and the following disclaimer in the
    documentation and/or other materials provided with the distribution.
  * Neither the name of the software's owners nor the names of its
    contributors may be used to endorse or promote products derived from
    this software without specific prior written permission.
  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
  "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
  LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
  A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
  OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
  SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
  LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
  DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
  THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
  (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
  OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

  (This is the Modified BSD License)
*/


#ifndef VARYINGREF_H
#define VARYINGREF_H


/// VaryingRef is a templated class (on class T) that holds either a
/// pointer to a single T value, or an "array" of T values, each
/// separated by a certain number of bytes.  For those versed in the
/// lingo of SIMD shading, this encapsulates 'uniform' and 'varying'
/// references.
///
/// Suppose you have a computation 'kernel' that is performing an
/// operation while looping over several computation 'points.'  Each of
/// the several operands of the kernel may either be a 'uniform' value
/// (identical for each point), or 'varying' (having a potentially
/// different value for each point).
///
/// Here is a concrete example.  Suppose you have the following function:
///     void add (int n, float *a, float *b, float *result) {
///         for (int i = 0;  i < n;  ++i)
///             result[i] = a[i] + b[i];
///     }
/// But if the caller of this function has only a single b value (let's
/// say, you always want to add 3 to every b[i]), you would be forced
/// to replicate an entire array full of 3's in order to call the function.
///
/// Instead, we may wish to generalize the function so that each operand
/// may rever to EITHER a single value or an array of values, without
/// making the code more complicated.  We can do this with VaryingRef:
///     void add (int n, VaryingRef<float> a, VaryingRef<float> b,
///                      VaryingRef<float> result) {
///         for (int i = 0;  i < n;  ++i)
///             result[i] = a[i] + b[i];
///     }
/// VaryingRef overloads operator [] to properly decode whether it is 
/// uniform (point to the one value) or varying (index the right array
/// element).  It also overloads the increment operator ++ and the pointer
/// indirection operator '*', so you could also write the function 
/// equivalently as:
///     void add (int n, VaryingRef<float> a, VaryingRef<float> b,
///                      VaryingRef<float> result) {
///         for (int i = 0;  i < n;  ++i, ++a, ++b)   // note increments
///             result[i] = (*a) + (*b);
///     }
///
/// An example of calling this function would be:
///     float a[n];
///     float b;     // just 1 value
///     float result[n];
///     add (n, VaryingRef<float>(a,sizeof(a)), VaryingRef<float>(b), result);
/// In this example, we're passing a truly varying 'a' (signified by
/// giving a step size from element to element), but a uniform 'b' (signified
/// by no step size, or a step size of zero).
///
/// Now let's take it a step further and fully optimize the 'add' function
/// for when both operands are uniform:
///     void add (int n, VaryingRef<float> a, VaryingRef<float> b,
///                      VaryingRef<float> result) {
///         if (a.is_uniform() && b.is_uniform()) {
///             float r = (*a) + (*b);
///             for (int i = 0;  i < n;  ++i)
///                 result[i] = r;
///         } else {
///             // One or both are varying
///             for (int i = 0;  i < n;  ++i, ++a, ++b)
///                 result[i] = (*a) + (*b);
///         }
///     }
/// This is the basis for handling uniform and varying values efficiently
/// inside a SIMD shading system.

template<class T>
class VaryingRef {
public:
    VaryingRef () { }

    /// Construct a VaryingRef either of a single value pointed to by ptr
    /// (if step == 0 or no step is provided), or of a varying set of
    /// values beginning with ptr and with successive values every 'step'
    /// bytes.
    VaryingRef (T *ptr, int step=0) { init (ptr,step); }

    /// Construct a uniform VaryingRef from a single value.
    ///
    VaryingRef (T &ptr) { init (&ptr, 0); }

    /// Initialize this VaryingRef to either of a single value pointed
    /// to by ptr (if step == 0 or no step is provided), or of a varying
    /// set of values beginning with ptr and with successive values
    /// every 'step' bytes.
    void init (T *ptr, int step=0) {
        m_ptr = ptr;
        m_step = step;
    }

    /// Initialize this VaryingRef to be uniform and point to a particular
    /// value reference.
    const VaryingRef & operator= (T &ptr) { init (&ptr); return *this; }

    /// Is this reference pointing nowhere?
    ///
    bool is_null () const { return (m_ptr == 0); }

    /// Cast to bool returns 'true' if the reference points to anything,
    /// or 'false' of it just holds a NULL pointer.
    operator bool() const { return (m_ptr != 0); }

    /// Is this VaryingRef referring to a varying value, signified by
    /// having a nonzero step size between elements?
    bool is_varying () const { return (m_step != 0); }

    /// Is this VaryingRef referring to a uniform value, signified by
    /// having a step size of zero between elements?
    bool is_uniform () const { return (m_step == 0); }

    /// Pre-increment: If this VaryingRef is varying, increment its
    /// pointer to the next element in the series, but don't change
    /// anything if it's uniform.  In either case, return a reference to
    /// its new state.
    VaryingRef & operator++ () {  // Prefix form ++i
        *((char **)&m_ptr) += m_step;
        return *this;
    }
    /// Post-increment: If this VaryingRef is varying, increment its
    /// pointer to the next element in the series, but don't change
    /// anything if it's uniform.  No value is returned, so it's not
    /// legal to do 'bar = foo++' if foo and bar are VaryingRef's.
    void operator++ (int) {  // Postfix form i++ : return nothing to avoid copy
        // VaryingRef<T> tmp = *this;
        *((char **)&m_ptr) += m_step;
        // return tmp;
    }

    /// Pointer indirection will return the first value currently
    /// pointed to by this VaryingRef.
    T & operator* () const { return *m_ptr; }

    /// Array indexing operator will return a reference to the single
    /// element if *this is uniform, or to the i-th element of the
    /// series if *this is varying.
    T & operator[] (int i) const { return *(T *) ((char *)m_ptr + i*m_step); }

    /// Return the raw pointer underneath.
    ///
    T * ptr () const { return m_ptr; }

private:
    T  *m_ptr;    ///< Pointer to value
    int m_step;   ///< Distance between successive values -- in BYTES!
};



/// Helper function wraps a varying reference with default step size.
///
template<class T>
VaryingRef<T> Varying (T *x) { return VaryingRef<T> (x, sizeof(T)); }

/// Helper function wraps a uniform reference.
///
template<class T>
VaryingRef<T> Uniform (T *x) { return VaryingRef<T> (x, 0); }

template<class T>
VaryingRef<T> Uniform (T &x) { return VaryingRef<T> (&x, 0); }


#endif // VARYINGREF_H
