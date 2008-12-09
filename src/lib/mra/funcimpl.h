/*
  This file is part of MADNESS.

  Copyright (C) <2007> <Oak Ridge National Laboratory>

  This program is free software; you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation; either version 2 of the License, or
  (at your option) any later version.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program; if not, write to the Free Software
  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307 USA

  For more information please contact:

  Robert J. Harrison
  Oak Ridge National Laboratory
  One Bethel Valley Road
  P.O. Box 2008, MS-6367

  email: harrisonrj@ornl.gov
  tel:   865-241-3937
  fax:   865-572-0680


  $Id$
*/


#ifndef MAD_FUNC_DATA
#define MAD_FUNC_DATA

/// \file funcimpl.h
/// \brief Provides FunctionCommonData, FunctionImpl and FunctionFactory

#include <iostream>
#include <world/world.h>
#include <world/print.h>
#include <misc/misc.h>
#include <tensor/tensor.h>
#include <mra/key.h>
#include <mra/funcdefaults.h>

namespace madness {
    template <typename T, int NDIM> class FunctionImpl;
    template <typename T, int NDIM> class Function;
    template <int D> class LoadBalImpl;
//    template <int D> class LBTreeImpl;
    template <int D> class LBTree;
    template <int D> class MyPmap;
}


namespace madness {


    /// A simple process map soon to be supplanted by Rebecca's
    template <typename keyT>
    class SimpleMap : public WorldDCPmapInterface< keyT > {
    private:
        const int nproc;
        const ProcessID me;
        const int n;

    public:
        SimpleMap(World& world, int n = 4) : nproc(world.nproc()), me(world.rank()), n(n) {}

        ProcessID owner(const keyT& key) const {
            if (key.level() == 0) {
                return 0;
            }
            else if (key.level() <= n) {
                return hash(key)%nproc;
            }
            else {
                return hash(key.parent(key.level()-n))%nproc;
            }
        }
    };


    /// FunctionCommonData holds all Function data common for given k

    /// Since Function assignment and copy constructors are shallow it
    /// greatly simplifies maintaining consistent state to have all
    /// (permanent) state encapsulated in a single class.  The state
    /// is shared between instances using a SharedPtr.  Also,
    /// separating shared from instance specific state accelerates the
    /// constructor, which is important for massive parallelism, and
    /// permitting inexpensive use of temporaries.  The default copy
    /// constructor and assignment operator are used but are probably
    /// never invoked.
    template <typename T, int NDIM>
    class FunctionCommonData {
    private:
        static FunctionCommonData<T,NDIM> data[MAXK+1]; /// Declared in mra.cc, initialized on first use

        /// Private.  Make the level-0 blocks of the periodic central difference derivative operator
        void _make_dc_periodic();

        /// Private.  Initialize the twoscale coefficients
        void _init_twoscale();

        /// Private.  Do first use initialization
        void _initialize(int k) {
            this->k = k;
            npt = k;
            for (int i=0; i<4; i++)
                s[i] = Slice(i*k,(i+1)*k-1);
            s0 = std::vector<Slice>(NDIM);
            sh = std::vector<Slice>(NDIM);
            vk = std::vector<long>(NDIM);
            vq = std::vector<long>(NDIM);
            v2k = std::vector<long>(NDIM);
            for (int i=0; i<NDIM; i++) {
                s0[i] = s[0];
                sh[i] = Slice(0,(k-1)/2);
                vk[i] = k;
                vq[i] = npt;
                v2k[i] = 2*k;
            }
            zero_coeff = tensorT(vk);
            key0 = Key<NDIM>(0,Vector<Translation,NDIM>(0));

            _init_twoscale();
            _init_quadrature(k, npt, quad_x, quad_w, quad_phi, quad_phiw, quad_phit);
            _make_dc_periodic();
            initialized = true;
        }

        FunctionCommonData()
            : initialized(false)
        {}

        bool initialized;
    public:
        typedef Tensor<T> tensorT; ///< Type of tensor used to hold coeff

        int k;                  ///< order of the wavelet
        int npt;                ///< no. of quadrature points
        Slice s[4];             ///< s[0]=Slice(0,k-1), s[1]=Slice(k,2*k-1), etc.
        std::vector<Slice> s0;  ///< s[0] in each dimension to get scaling coeff
        std::vector<Slice> sh;  ///< Slice(0,(k-1)/2) in each dimension for autorefine test
        std::vector<long> vk;   ///< (k,...) used to initialize Tensors
        std::vector<long> v2k;  ///< (2k,...) used to initialize Tensors
        std::vector<long> vq;   ///< (npt,...) used to initialize Tensors

        tensorT zero_coeff;     ///< Zero (k,...) tensor for internal convenience of diff

        Key<NDIM> key0;         ///< Key for root node

        Tensor<double> quad_x;  ///< quadrature points
        Tensor<double> quad_w;  ///< quadrature weights
        Tensor<double> quad_phi; ///< quad_phi(i,j) = at x[i] value of phi[j]
        Tensor<double> quad_phit; ///< transpose of quad_phi
        Tensor<double> quad_phiw; ///< quad_phiw(i,j) = at x[i] value of w[i]*phi[j]

        Tensor<double> h0, h1, g0, g1; ///< The separate blocks of twoscale coefficients
        Tensor<double> hg, hgT; ///< The full twoscale coeff (2k,2k) and transpose
        Tensor<double> hgsonly; ///< hg[0:k,:]

        Tensor<double> rm, r0, rp;        ///< Blocks of the derivative operator
        Tensor<double> rm_left, rm_right, rp_left, rp_right; ///< Rank-1 forms rm & rp

        static const FunctionCommonData<T,NDIM>& get(int k) {
            MADNESS_ASSERT(k>0 && k<=MAXK);
            if (!data[k].initialized) data[k]._initialize(k);
            return data[k];
        }

        /// Initialize the quadrature information

        /// Made public with all arguments thru interface for reuse in FunctionImpl::err_box
        static void _init_quadrature(int k, int npt,
                                     Tensor<double>& quad_x, Tensor<double>& quad_w,
                                     Tensor<double>& quad_phi, Tensor<double>& quad_phiw,
                                     Tensor<double>& quad_phit);
    };

    /// Interface required for functors used as input to Functions
    template <typename T, int NDIM>
    class FunctionFunctorInterface {
    public:
        virtual T operator()(const Vector<double,NDIM>& x) const = 0;
	virtual ~FunctionFunctorInterface() {}
    };

    /// FunctionFactory implements the named-parameter idiom for Function

    /// C++ does not provide named arguments (as does, e.g., Python).
    /// This class provides something very close.  Create functions as follows
    /// \code
    /// double myfunc(const double x[]);
    /// Function<double,3> f = FunctionFactory<double,3>(world).f(myfunc).k(11).thresh(1e-9).debug()
    /// \endcode
    /// where the methods of function factory, which specify the non-default
    /// arguments eventually passed to the \c Function constructor, can be
    /// used in any order.
    ///
    /// Need to add a general functor for initial projection with a standard interface.
    template <typename T, int NDIM>
    class FunctionFactory {
        friend class FunctionImpl<T,NDIM>;
        typedef Vector<double,NDIM> coordT;            ///< Type of vector holding coordinates
    protected:
        World& _world;
        int _k;
        double _thresh;
        int _initial_level;
        int _max_refine_level;
        int _truncate_mode;
        bool _refine;
        bool _empty;
        bool _autorefine;
        bool _truncate_on_project;
        bool _fence;
        Tensor<int> _bc;
        SharedPtr< WorldDCPmapInterface< Key<NDIM> > > _pmap;
        SharedPtr< FunctionFunctorInterface<T,NDIM> > _functor;

        struct FunctorInterfaceWrapper : public FunctionFunctorInterface<T,NDIM> {
            T (*f)(const coordT&);

            FunctorInterfaceWrapper(T (*f)(const coordT&)) : f(f) {};

            T operator()(const coordT& x) const {return f(x);};
        };

    public:
        FunctionFactory(World& world)
            : _world(world)
            , _k(FunctionDefaults<NDIM>::get_k())
            , _thresh(FunctionDefaults<NDIM>::get_thresh())
            , _initial_level(FunctionDefaults<NDIM>::get_initial_level())
            , _max_refine_level(FunctionDefaults<NDIM>::get_max_refine_level())
            , _truncate_mode(FunctionDefaults<NDIM>::get_truncate_mode())
            , _refine(FunctionDefaults<NDIM>::get_refine())
            , _empty(false)
            , _autorefine(FunctionDefaults<NDIM>::get_autorefine())
            , _truncate_on_project(FunctionDefaults<NDIM>::get_truncate_on_project())
            , _fence(true)
            , _bc(FunctionDefaults<NDIM>::get_bc())
            , _pmap(FunctionDefaults<NDIM>::get_pmap())
            , _functor(0)
        {}
        FunctionFactory& functor(const SharedPtr< FunctionFunctorInterface<T,NDIM> >& functor) {
            _functor = functor;
            return *this;
        }
        FunctionFactory& f(T (*f)(const coordT&)) {
            functor(SharedPtr< FunctionFunctorInterface<T,NDIM> >(new FunctorInterfaceWrapper(f)));
            return *this;
        }
        FunctionFactory& k(int k) {
            _k = k;
            return *this;
        }
        FunctionFactory& thresh(double thresh) {
            _thresh = thresh;
            return *this;
        }
        FunctionFactory& initial_level(int initial_level) {
            _initial_level = initial_level;
            return *this;
        }
        FunctionFactory& max_refine_level(int max_refine_level) {
            _max_refine_level = max_refine_level;
            return *this;
        }
        FunctionFactory& truncate_mode(int truncate_mode) {
            _truncate_mode = truncate_mode;
            return *this;
        }
        FunctionFactory& refine(bool refine = true) {
            _refine = refine;
            return *this;
        }
        FunctionFactory& norefine(bool norefine = true) {
            _refine = !norefine;
            return *this;
        }
        FunctionFactory& bc(const Tensor<int>& bc) {
            _bc = copy(bc);
            return *this;
        }
        FunctionFactory& empty() {
            _empty = true;
            return *this;
        }
        FunctionFactory& autorefine() {
            _autorefine = true;
            return *this;
        }
        FunctionFactory& noautorefine() {
            _autorefine = false;
            return *this;
        }
        FunctionFactory& truncate_on_project() {
            _truncate_on_project = true;
            return *this;
        }
        FunctionFactory& notruncate_on_project() {
            _truncate_on_project = false;
            return *this;
        }
        FunctionFactory& fence(bool fence=true) {
            _fence = fence;
            return *this;
        }
        FunctionFactory& nofence() {
            _fence = false;
            return *this;
        }
        FunctionFactory& pmap(const SharedPtr< WorldDCPmapInterface< Key<NDIM> > >& pmap) {
            _pmap = pmap;
            return *this;
        }
    };

    /// FunctionNode holds the coefficients, etc., at each node of the 2^NDIM-tree
    template <typename T, int NDIM>
    class FunctionNode {
    private:
        // Should compile OK with these volatile but there should
        // be no need to set as volatile since the container internally
        // stores the entire entry as volatile

        Tensor<T> _coeffs;  ///< The coefficients, if any
        double _norm_tree;  ///< After norm_tree will contain norm of coefficients summed up tree
        bool _has_children; ///< True if there are children

    public:
        typedef WorldContainer< Key<NDIM>, FunctionNode<T,NDIM> > dcT;  ///< Type of container holding the nodes
        /// Default constructor makes node without coeff or children
        FunctionNode()
            : _coeffs()
            , _norm_tree(1e300)
            , _has_children(false)
        {}

        /// Constructor from given coefficients with optional children

        /// Note that only a shallow copy of the coeff are taken so
        /// you should pass in a deep copy if you want the node to
        /// take ownership.
        explicit FunctionNode(const Tensor<T>& coeff, bool has_children=false)
            : _coeffs(coeff)
            , _norm_tree(1e300)
            , _has_children(has_children)
        {}

        FunctionNode(const FunctionNode<T,NDIM>& other) {
            *this = other;
        }

        FunctionNode<T,NDIM>& operator=(const FunctionNode<T,NDIM>& other) {
            if (this != &other) {
                coeff() = copy(other.coeff());
                _norm_tree = other._norm_tree;
                _has_children = other._has_children;
            }
            return *this;
        }

        /// Copy with possible type conversion of coefficients, copying all other state

        /// Choose to not overload copy and type conversion operators
        /// so there are no automatic type conversions.
        template <typename Q>
        FunctionNode<Q,NDIM> convert() const {
            return FunctionNode<Q,NDIM>(copy(coeff()),_has_children);
        }

        /// Returns true if there are coefficients in this node
        bool has_coeff() const {return (_coeffs.size>0);}

        /// Returns true if this node has children
        bool has_children() const {return _has_children;}

        /// Returns true if this does not have children
        bool is_leaf() const {return !_has_children;}

        /// Returns true if this node is invalid (no coeffs and no children)
        bool is_invalid() const {return !(has_coeff() || has_children());}

        /// Returns a non-const reference to the tensor containing the coeffs

        /// Returns an empty tensor if there are no coefficients.
        Tensor<T>& coeff() {
            MADNESS_ASSERT(_coeffs.ndim==-1 || (_coeffs.dim[0]<=2*MAXK && _coeffs.dim[0]>=0));
            return const_cast< Tensor<T>& >(_coeffs);}

        /// Returns a const reference to the tensor containing the coeffs

        /// Returns an empty tensor if there are no coefficeints.
        const Tensor<T>& coeff() const {return const_cast< const Tensor<T>& >(_coeffs);}

        /// Sets \c has_children attribute to value of \c flag.
        Void set_has_children(bool flag) {_has_children = flag; return None;}

        /// Sets \c has_children attribute to true recurring up to ensure connected
        Void set_has_children_recursive(const typename FunctionNode<T,NDIM>::dcT& c, const Key<NDIM>& key) {
            PROFILE_MEMBER_FUNC(FunctionNode);
            if (!(_has_children || has_coeff() || key.level()==0)) {
                // If node already knows it has children or it has
                // coefficients then it must already be connected to
                // its parent.  If not, the node was probably just
                // created for this operation and must be connected to
                // its parent.
                Key<NDIM> parent = key.parent();
                const_cast<dcT&>(c).task(parent, &FunctionNode<T,NDIM>::set_has_children_recursive, c, parent, TaskAttributes::hipri());
            }
            _has_children = true;
            return None;
        }

        /// Sets \c has_children attribute to value of \c !flag
        void set_is_leaf(bool flag) {_has_children = !flag;}

        /// Takes a \em shallow copy of the coeff --- same as \c this->coeff()=coeff
        void set_coeff(const Tensor<T>& coeffs) {
            coeff() = coeffs;
	    if ((_coeffs.dim[0] < 0) || (_coeffs.dim[0]>2*MAXK)) {
                print("set_coeff: may have a problem");
                print("set_coeff: coeff.dim[0] =", coeffs.dim[0], ", 2* MAXK =", 2*MAXK);
	    }
            MADNESS_ASSERT(coeffs.dim[0]<=2*MAXK && coeffs.dim[0]>=0);
        }

        /// Clears the coefficients (has_coeff() will subsequently return false)
        void clear_coeff() {coeff() = Tensor<T>();}

        /// Sets the value of norm_tree
        Void set_norm_tree(double norm_tree) {_norm_tree = norm_tree; return None;}

        /// Gets the value of norm_tree
        double get_norm_tree() const {return _norm_tree;}

        /// General bi-linear operation --- this = this*alpha + other*beta

        /// This/other may not have coefficients.  Has_children will be
        /// true in the result if either this/other have children.
        template <typename Q, typename R>
        Void gaxpy_inplace(const T& alpha, const FunctionNode<Q,NDIM>& other, const R& beta) {
            PROFILE_MEMBER_FUNC(FuncNode);
            if (other.has_children()) _has_children = true;
            if (has_coeff()) {
                if (other.has_coeff()) {
                    coeff().gaxpy(alpha,other.coeff(),beta);
                }
                else {
                    coeff().scale(alpha);
                }
            }
            else if (other.has_coeff()) {
                coeff() = other.coeff()*beta; //? Is this the correct type conversion?
            }
            return None;
        }


        /// Accumulate inplace and if necessary connect node to parent
        Void accumulate(const Tensor<T>& t, const typename FunctionNode<T,NDIM>::dcT& c, const Key<NDIM>& key) {
            if (has_coeff()) {
                coeff() += t;
            }
            else {
                // No coeff and no children means the node is newly
                // created for this operation and therefore we must
                // tell its parent that it exists.
                coeff() = copy(t);
                if ((!_has_children) && key.level() > 0) {
                    Key<NDIM> parent = key.parent();
                    const_cast<dcT&>(c).task(parent, &FunctionNode<T,NDIM>::set_has_children_recursive, c, parent, TaskAttributes::hipri());
                }
            }
            return None;
        }

        template <typename Archive>
        void serialize(Archive& ar) {
            ar & coeff() & _has_children & double(_norm_tree);
        }
    };

    template <typename T, int NDIM>
    std::ostream& operator<<(std::ostream& s, const FunctionNode<T,NDIM>& node) {
        s << "(" << node.has_coeff() << ", " << node.has_children() << ", ";
        double norm = node.has_coeff() ? node.coeff().normf() : 0.0;
        if (norm < 1e-12) norm = 0.0;
        s << norm << ")";
        return s;
    }

    /// ApplyTime is a class for finding out the time taken in the apply
    /// function.

    template <int NDIM>
    class ApplyTime {
      typedef Key<NDIM> keyT;
      typedef WorldContainer<keyT, double> dcT;
      typedef std::pair<const keyT, double> datumT;

    private:
      World& world;
      dcT hash_table;
      double decay_val;

    public:
      ApplyTime(World& world)
	: world(world)
	, hash_table(dcT(world))
	, decay_val(0.9)
	{}

      void set(datumT data) {
	hash_table.replace(data);
      }

      void clear() {
	hash_table.clear();
      }

      double get(keyT& key) {
	typename dcT::iterator it = hash_table.find(key);
	if (it == hash_table.end()) {
	  return 0.0;
	}
	else {
	  return it->second;
	}
      }

      double get(const keyT& key) {
	typename dcT::iterator it = hash_table.find(key);
	if (it == hash_table.end()) {
	  return 0.0;
	}
	else {
	  return it->second;
	}
      }

      /* datumT get(keyT& key) { */
/* 	double result = this->get(key); */
/* 	return datumT(key, result); */
/*       } */

      void update(datumT data) {
	typename dcT::iterator it = hash_table.find(data.first).get();
	if (it == hash_table.end()) {
	  hash_table.replace(data);
	}
	else {
	  double s = it->second, y = data.second;
	  data.second = s + (y-s)*decay_val;
	  hash_table.replace(data);
	}
      }

      void update(keyT key, double d) {
	update(datumT(key, d));
      }

      void print() {
	for (typename dcT::iterator it = hash_table.begin(); it != hash_table.end(); ++it) {
	  madness::print(it->first, "  ", it->second);
	}
      }

    };



    /// FunctionImpl holds all Function state to facilitate shallow copy semantics

    /// Since Function assignment and copy constructors are shallow it
    /// greatly simplifies maintaining consistent state to have all
    /// (permanent) state encapsulated in a single class.  The state
    /// is shared between instances using a SharedPtr<FunctionImpl>.
    ///
    /// The FunctionImpl inherits all of the functionality of WorldContainer
    /// (to store the coefficients) and WorldObject<WorldContainer> (used
    /// for RMI and for its unqiue id).
    template <typename T, int NDIM>
    class FunctionImpl : public WorldObject< FunctionImpl<T,NDIM> > {
    public:
	//friend class Function<T,NDIM>;
        template <typename Q, int D> friend class Function;
        template <typename Q, int D> friend class FunctionImpl;
	friend class LoadBalImpl<NDIM>;
	friend class LBTree<NDIM>;

        typedef FunctionImpl<T,NDIM> implT;       ///< Type of this class (implementation)
        typedef Tensor<T> tensorT;                     ///< Type of tensor used to hold coeffs
        typedef Vector<Translation,NDIM> tranT;         ///< Type of array holding translation
        typedef Key<NDIM> keyT;                        ///< Type of key
        typedef FunctionNode<T,NDIM> nodeT;            ///< Type of node
        typedef WorldContainer<keyT,nodeT> dcT;  ///< Type of container holding the coefficients
        typedef std::pair<const keyT,nodeT> datumT;    ///< Type of entry in container
        typedef Vector<double,NDIM> coordT;            ///< Type of vector holding coordinates

        World& world;
    private:
        int k;                  ///< Wavelet order
        double thresh;          ///< Screening threshold
        int initial_level;      ///< Initial level for refinement
        int max_refine_level;   ///< Do not refine below this level
        int truncate_mode;      ///< 0=default=(|d|<thresh), 1=(|d|<thresh/2^n), 1=(|d|<thresh/4^n);
        bool autorefine;        ///< If true, autorefine where appropriate
        bool truncate_on_project; ///< If true projection inserts at level n-1 not n
        bool nonstandard;       ///< If true, compress keeps scaling coeff

        const FunctionCommonData<T,NDIM>& cdata;

        SharedPtr< FunctionFunctorInterface<T,NDIM> > functor;

        bool compressed;        ///< Compression status

        dcT coeffs;             ///< The coefficients

        Tensor<int> bc;     ///< Type of boundary condition -- currently only zero or periodic

	SharedPtr<ApplyTime<NDIM> > apply_time;

        // Disable the default copy constructor
        FunctionImpl(const FunctionImpl<T,NDIM>& p);

    public:

        /// Initialize function impl from data in factory
        FunctionImpl(const FunctionFactory<T,NDIM>& factory)
            : WorldObject<implT>(factory._world)
            , world(factory._world)
            , k(factory._k)
            , thresh(factory._thresh)
            , initial_level(factory._initial_level)
            , max_refine_level(factory._max_refine_level)
            , truncate_mode(factory._truncate_mode)
            , autorefine(factory._autorefine)
            , truncate_on_project(factory._truncate_on_project)
            , nonstandard(false)
            , cdata(FunctionCommonData<T,NDIM>::get(k))
            , functor(factory._functor)
            , compressed(false)
            , coeffs(world,factory._pmap,false)
            , bc(factory._bc)
	    , apply_time(SharedPtr<ApplyTime<NDIM> > ())
        {
            PROFILE_MEMBER_FUNC(FunctionImpl);
            // !!! Ensure that all local state is correctly formed
            // before invoking process_pending for the coeffs and
            // for this.  Otherwise, there is a race condition.
            MADNESS_ASSERT(k>0 && k<=MAXK);

            bool empty = factory._empty;
            bool do_refine = factory._refine;

            if (do_refine) initial_level = std::max(0,initial_level - 1);

            if (empty) {        // Do not set any coefficients at all
            }
            else if (functor) { // Project function and optionally refine
                insert_zero_down_to_initial_level(cdata.key0);
                for (typename dcT::iterator it=coeffs.begin(); it!=coeffs.end(); ++it) {
                    if (it->second.is_leaf())
                        task(coeffs.owner(it->first), &implT::project_refine_op, it->first, do_refine);
                }
            }
            else {  // Set as if a zero function
                initial_level = 1;
                insert_zero_down_to_initial_level(keyT(0));
            }

            coeffs.process_pending();
            this->process_pending();
            if (factory._fence && functor) world.gop.fence();
        }

        /// Copy constructor

        /// Allocates a \em new function in preparation for a deep copy
        ///
        /// By default takes pmap from other but can also specify a different pmap.
        /// Does \em not copy the coefficients ... creates an empty container.
        template <typename Q>
        FunctionImpl(const FunctionImpl<Q,NDIM>& other,
                     const SharedPtr< WorldDCPmapInterface< Key<NDIM> > >& pmap,
                     bool dozero)
            : WorldObject<implT>(other.world)
            , world(other.world)
            , k(other.k)
            , thresh(other.thresh)
            , initial_level(other.initial_level)
            , max_refine_level(other.max_refine_level)
            , truncate_mode(other.truncate_mode)
            , autorefine(other.autorefine)
            , truncate_on_project(other.truncate_on_project)
            , nonstandard(other.nonstandard)
            , cdata(FunctionCommonData<T,NDIM>::get(k))
            , functor()
            , compressed(other.compressed)
            , coeffs(world, pmap ? pmap : other.coeffs.get_pmap())
            , bc(other.bc)
	  , apply_time(other.apply_time)
        {
            if (dozero) {
                initial_level = 1;
                insert_zero_down_to_initial_level(cdata.key0);
            }
            coeffs.process_pending();
            this->process_pending();
        }

	const SharedPtr< WorldDCPmapInterface< Key<NDIM> > >& get_pmap() const {
	    return coeffs.get_pmap();
	}

        /// Copy coeffs from other into self
        template <typename Q>
	void copy_coeffs(const FunctionImpl<Q,NDIM>& other, bool fence) {
	    for (typename FunctionImpl<Q,NDIM>::dcT::const_iterator it=other.coeffs.begin();
                it!=other.coeffs.end(); ++it) {
                const keyT& key = it->first;
                const typename FunctionImpl<Q,NDIM>::nodeT& node = it->second;
		coeffs.replace(key,node. template convert<Q>());
	    }
	    if (fence) world.gop.fence();
	}

        template <typename Q, typename R>
        struct do_gaxpy_inplace {
            typedef Range<typename FunctionImpl<Q,NDIM>::dcT::const_iterator> rangeT;
            FunctionImpl<T,NDIM>* f;
            T alpha;
            R beta;
            do_gaxpy_inplace() {};
            do_gaxpy_inplace(FunctionImpl<T,NDIM>* f, T alpha, R beta) : f(f), alpha(alpha), beta(beta) {}
            bool operator() (typename rangeT::iterator& it) const {
                const keyT& key = it->first;
                const FunctionNode<Q,NDIM>& other_node = it->second;
                // Use send to get write accessor and automated construction if missing
                f->coeffs.send(key, &nodeT:: template gaxpy_inplace<Q,R>, alpha, other_node, beta);
                return true;
            }
            template <typename Archive>
            void serialize(Archive& ar) {};
        };


        /// Inplace general bilinear operation
        template <typename Q, typename R>
        void gaxpy_inplace(const T& alpha,const FunctionImpl<Q,NDIM>& other, const R& beta, bool fence) {
            if (get_pmap() == other.get_pmap()) {
                typedef Range<typename FunctionImpl<Q,NDIM>::dcT::const_iterator> rangeT;
                typedef do_gaxpy_inplace<Q,R> opT;
                world.taskq.for_each<rangeT,opT>(rangeT(other.coeffs.begin(), other.coeffs.end()), opT(this, alpha, beta));
            }
            else {
                // Loop over coefficients in other that are local and then send an AM to
                // coeffs in self ... this is so can efficiently add functions with
                // different distributions.  Use an AM rather than a task to reduce memory
                // footprint on the remote end.
                for (typename FunctionImpl<Q,NDIM>::dcT::const_iterator it=other.coeffs.begin();
                     it!=other.coeffs.end();
                     ++it) {
                    const keyT& key = it->first;
                    const typename FunctionImpl<Q,NDIM>::nodeT& other_node = it->second;
                    coeffs.send(key, &nodeT:: template gaxpy_inplace<Q,R>, alpha, other_node, beta);
                }
            }
            if (fence) world.gop.fence();
	}

        template <typename Archive>
        void load(Archive& ar) {
            int kk;
            ar & kk;

            MADNESS_ASSERT(kk==k);

            // note that functor should not be (re)stored
            ar & thresh & initial_level & max_refine_level & truncate_mode
                & autorefine & truncate_on_project & nonstandard & compressed & bc;

            ar & coeffs;
            world.gop.fence();
        }


        template <typename Archive>
        void store(Archive& ar) {
            // note that functor should not be (re)stored
            ar & k & thresh & initial_level & max_refine_level & truncate_mode
                & autorefine & truncate_on_project & nonstandard & compressed & bc;

            ar & coeffs;
            world.gop.fence();
        }



        /// Returns true if the function is compressed.
        bool is_compressed() const {return compressed;}

        /// Adds a constant to the function.  Local operation, optional fence

        /// In scaling function basis must add value to first polyn in
        /// each box with appropriate scaling for level.  In wavelet basis
        /// need only add at level zero.
        void add_scalar_inplace(T t, bool fence);


        /// Initialize nodes to zero function at initial_level of refinement.

        /// Works for either basis.  No communication.
        void insert_zero_down_to_initial_level(const keyT& key);


        /// Truncate according to the threshold with optional global fence

        /// If thresh<=0 the default value of this->thresh is used
        void truncate(double tol, bool fence) {
            // Cannot put tol into object since it would make a race condition
            if (tol <= 0.0) tol = thresh;
            if (world.rank() == coeffs.owner(cdata.key0)) truncate_spawn(cdata.key0,tol);
            if (fence) world.gop.fence();
        }


        /// Returns true if after truncation this node has coefficients

        /// Assumed to be invoked on process owning key.  Possible non-blocking
        /// communication.
        Future<bool> truncate_spawn(const keyT& key, double tol);

        /// Actually do the truncate operation
        bool truncate_op(const keyT& key, double tol, const std::vector< Future<bool> >& v);

        /// Evaluate function at quadrature points in the specified box
        void fcube(const keyT& key, const FunctionFunctorInterface<T,NDIM>& f, const Tensor<double>& qx, tensorT& fval) const;

        const keyT& key0() const {
            return cdata.key0;
        }

        void print_tree(Level maxlevel = 10000) const;

        void do_print_tree(const keyT& key, Level maxlevel) const;

        /// Compute by projection the scaling function coeffs in specified box
        tensorT project(const keyT& key) const;


        /// Returns the truncation threshold according to truncate_method
        double truncate_tol(double tol, const keyT& key) const {
            if (truncate_mode == 0) {
                return tol;
            }
            else if (truncate_mode == 1) {
                double L = FunctionDefaults<NDIM>::get_cell_min_width();
                return tol*std::min(1.0,pow(0.5,double(key.level()))*L);
            }
            else if (truncate_mode == 2) {
                double L = FunctionDefaults<NDIM>::get_cell_min_width();
                return tol*std::min(1.0,pow(0.25,double(key.level()))*L*L);
            }
            else {
                MADNESS_EXCEPTION("truncate_mode invalid",truncate_mode);
           }
        }


        /// Returns patch referring to coeffs of child in parent box
        std::vector<Slice> child_patch(const keyT& child) const {
            std::vector<Slice> s(NDIM);
            const Vector<Translation,NDIM>& l = child.translation();
            for (int i=0; i<NDIM; i++) s[i] = cdata.s[l[i]&1]; // Lowest bit of translation
            return s;
        }


        /// Projection with optional refinement
        Void project_refine_op(const keyT& key, bool do_refine);


        /// Compute the Legendre scaling functions for multiplication

        /// Evaluate parent polyn at quadrature points of a child.  The prefactor of
        /// 2^n/2 is included.  The tensor must be preallocated as phi(k,npt).
        /// Refer to the implementation notes for more info.
        void phi_for_mul(Level np, Translation lp, Level nc, Translation lc, Tensor<double>& phi) const;

        /// Directly project parent coeffs to child coeffs

        /// Currently used by diff, but other uses can be anticipated
        const tensorT parent_to_child(const tensorT& s, const keyT& parent, const keyT& child) const;


        ///Change bv on the fly. Temporary workaround until better bc handling is introduced.
        void set_bc(const Tensor<int>& value) {bc=copy(value); MADNESS_ASSERT(bc.dim[0]==NDIM && bc.dim[1]==2 && bc.ndim==2);}

        /// Get the scaling function coeffs at level n starting from NS form
	// N=2^n, M=N/q, q must be power of 2
	// q=0 return coeffs [N,k] for direct sum
	// q>0 return coeffs [k,q,M] for fft sum
        tensorT coeffs_for_jun(Level n, long q=0) {
            MADNESS_ASSERT(compressed && nonstandard && NDIM<=3);
	    tensorT r,r0;
	    long N=1<<n;
	    long M = (q ? N/q: N);
	    if (q==0) {
	    	q = 1;
	    	long dim[2*NDIM];
            	for (int d=0; d<NDIM; d++) {
                	dim[d     ] = N;
                	dim[d+NDIM] = cdata.k;
		}
		tensorT rr(2*NDIM,dim);
		r0=r=rr;
		//NNkk->MqMqkk, since fuse is not allowed. Now needs to move back to 2*NDIM, since tensor max dim is 6
		//for (int d=NDIM-1; d>=0; --d) r.splitdim_inplace_base(d,M,q);
            } else {
	  	long dim[2*NDIM];
                for (int d=0; d<NDIM; d++) {
                        //dim[d+NDIM*2] = M;
                        dim[d+NDIM  ] = N;
			dim[d       ] = cdata.k;
                }
		tensorT rr(2*NDIM,dim);
		r0=r=rr;
		/*vector<long> map(3*NDIM);
		for (int d=0; d<NDIM; ++d) {
			map[d]=d+2*NDIM;
			map[NDIM+d]=2*d+1;
			map[2*NDIM+d]=2*d;
		}
		r.mapdim_inplace_base(map);
		//print(rr);
		//for (int d=1; d<NDIM; ++d) rr.swapdim_inplace_base(2*NDIM+d,NDIM+d); //kkqqMM->kkqMqM
		//print(rr);
		//for (int d=0; d<NDIM; ++d) rr.swapdim_inplace_base(NDIM+2*d,NDIM+2*d-1); //kkqMqM->kkMqMq
		//print(rr);
		//for (int d=0; d<NDIM; ++d) rr.fusedim_inplace_base(NDIM+d); //->kkNN
		//seems that this fuse is not allowed :(

		//print(rr);
		*/
		r.cycledim_inplace_base(NDIM,0,-1); //->NNkk or MqMqkk
	    }
	    //print("faking done M q r(fake) r0(real)",M,q,"\n", r,r0);
            ProcessID me = world.rank();
            Vector<long,NDIM> t(N);

	    Vector<long,NDIM> powq, powN, powM;
	    long NDIM1 = NDIM-1;
	    powM[NDIM1]=powq[NDIM1]=powN[NDIM1]=1;
	    for (int d=NDIM1-1; d>=0; --d) {
	    	powM[d] = powM[d+1]*M;
		powq[d] = powq[d+1]*q;
		powN[d] = powN[d+1]*N;
	    }
	    long powMNDIM = powM[0]*M;

            for (IndexIterator it(t); it; ++it) {
                keyT key(n, Vector<Translation,NDIM>(*it));
                if (coeffs.owner(key) == me) {
                    typename dcT::iterator it = coeffs.find(key).get();
		    tensorT qq;

		    if (it == coeffs.end()) {
                        // must get from above
                        typedef std::pair< keyT,Tensor<T> > pairT;
                        Future<pairT> result;
                        sock_it_to_me(key,  result.remote_ref(world));
                        const keyT& parent = result.get().first;
                        const tensorT& t = result.get().second;

                        qq = parent_to_child(t, parent, key);
                    }
                    else {
                        qq = it->second.coeff();
                    }
                    std::vector<Slice> s(NDIM*2);
		    long ll = 0;
                    for (int d=0; d<NDIM; d++) {
                        Translation l = key.translation()[d];
			long dum = float(l)/q;
			ll += (l - dum*q)*powMNDIM*powq[d] + dum*powM[d];
			//ll += (l % q)*powM[NDIM]*pow((double)q,NDIM-d-1) + (l/q)*pow((double)M,NDIM-d-1);

			//print("translation",l);
			//s[d       ] = Slice(l,l,0);
                        //s[d+NDIM  ] = Slice(l%q,l%q,0);
                        //s[d+NDIM] = Slice(0,k-1,1);
                    }
		    //long dum = ll;
		    for (int d=0; d<NDIM; d++) {
		    	Translation l = float(ll) / powN[d];
			//Translation l = ll / pow((double)N,NDIM-d-1);
			s[d     ] = Slice(l,l,0);
			s[d+NDIM] = Slice(0,k-1,1);
			ll = ll - l*powN[d];
			//ll = ll % long(pow((double)N,NDIM-d-1));
		    }
		    //print(s, dum, key.translation());
                    r(s) = qq(cdata.s0);

                }
            }

            world.gop.fence();
            world.gop.sum(r0);
	    //print(r,r0);

            return r0;
        }



        /// Compute the function values for multiplication

        /// Given coefficients from a parent cell, compute the value of
        /// the functions at the quadrature points of a child
        template <typename Q>
        Tensor<Q> fcube_for_mul(const keyT& child, const keyT& parent, const Tensor<Q>& coeff) const {
            PROFILE_MEMBER_FUNC(FunctionImpl);
            if (child.level() == parent.level()) {
                double scale = pow(2.0,0.5*NDIM*parent.level())/sqrt(FunctionDefaults<NDIM>::get_cell_volume());
                return transform(coeff,cdata.quad_phit).scale(scale);
            }
            else if (child.level() < parent.level()) {
                MADNESS_EXCEPTION("FunctionImpl: fcube_for_mul: child-parent relationship bad?",0);
            }
            else {
                Tensor<double> phi[NDIM];
                for (int d=0; d<NDIM; d++) {
                    phi[d] = Tensor<double>(cdata.k,cdata.npt);
                    phi_for_mul(parent.level(),parent.translation()[d],
                                child.level(), child.translation()[d], phi[d]);
                }
                return general_transform(coeff,phi).scale(1.0/sqrt(FunctionDefaults<NDIM>::get_cell_volume()));;
            }
        }

        /// Invoked as a task by mul with the actual coefficients
        template <typename L, typename R>
        Void do_mul(const keyT& key, const Tensor<L>& left, const std::pair< keyT, Tensor<R> >& arg) {
            PROFILE_MEMBER_FUNC(FunctionImpl);
            const keyT& rkey = arg.first;
            const Tensor<R>& rcoeff = arg.second;
            //madness::print("do_mul: r", rkey, rcoeff.size);
            Tensor<R> rcube = fcube_for_mul(key, rkey, rcoeff);
            //madness::print("do_mul: l", key, left.size);
            Tensor<L> lcube = fcube_for_mul(key,  key, left);

            Tensor<T> tcube(cdata.vk,false);
            TERNARY_OPTIMIZED_ITERATOR(T, tcube, L, lcube, R, rcube, *_p0 = *_p1 * *_p2;);
            double scale = pow(0.5,0.5*NDIM*key.level())*sqrt(FunctionDefaults<NDIM>::get_cell_volume());
            tcube = transform(tcube,cdata.quad_phiw).scale(scale);
            coeffs.replace(key, nodeT(tcube,false));
            return None;
        }


        /// Invoked by result to perform result += alpha*left+beta*right in wavelet basis

        /// Does not assume that any of result, left, right have the same distribution.
        /// For most purposes result will start as an empty so actually are implementing
        /// out of place gaxpy.  If all functions have the same distribution there is
        /// no communication except for the optional fence.
        template <typename L, typename R>
        void gaxpy(T alpha, const FunctionImpl<L,NDIM>& left,
                   T beta,  const FunctionImpl<R,NDIM>& right, bool fence) {
            // Loop over local nodes in both functions.  Add in left and subtract right.
            // Not that efficient in terms of memory bandwidth but ensures we do
            // not miss any nodes.
	    for (typename FunctionImpl<L,NDIM>::dcT::const_iterator it=left.coeffs.begin();
                it!=left.coeffs.end();
                 ++it) {
                const keyT& key = it->first;
                const typename FunctionImpl<L,NDIM>::nodeT& other_node = it->second;
                coeffs.send(key, &nodeT:: template gaxpy_inplace<T,L>, 1.0, other_node, alpha);
	    }
	    for (typename FunctionImpl<R,NDIM>::dcT::const_iterator it=right.coeffs.begin();
                 it!=right.coeffs.end();
                 ++it) {
                const keyT& key = it->first;
                const typename FunctionImpl<L,NDIM>::nodeT& other_node = it->second;
                coeffs.send(key, &nodeT:: template gaxpy_inplace<T,R>, 1.0, other_node, beta);
	    }
            if (fence) world.gop.fence();
        }


        template <typename opT>
        void unary_op_coeff_inplace(bool (implT::*refineop)(const keyT&, const tensorT&) const,
                                    const opT& op,
                                    bool fence) {
            throw "not working now";
        }


        /// Unary operation applied inplace to the coefficients WITHOUT refinement, optional fence
        template <typename opT>
        void unary_op_coeff_inplace(const opT& op, bool fence)
        {
            for (typename dcT::iterator it=coeffs.begin(); it!=coeffs.end(); ++it) {
                const keyT& parent = it->first;
                nodeT& node = it->second;
                if (node.has_coeff()) {
                    op(parent, node.coeff());
                }
            }
            if (fence) world.gop.fence();
        }


        /// Unary operation applied inplace to the coefficients WITHOUT refinement, optional fence
        template <typename opT>
        void unary_op_node_inplace(const opT& op, bool fence)
        {
            for (typename dcT::iterator it=coeffs.begin(); it!=coeffs.end(); ++it) {
                const keyT& parent = it->first;
                nodeT& node = it->second;
                op(parent, node);
            }
            if (fence) world.gop.fence();
        }


        template <typename opT>
        void unary_op_value_inplace(bool (implT::*refineop)(const keyT&, const tensorT&) const,
                                    const opT& op,
                                    bool fence) {

            throw "not working now";
        }

        template <typename opT>
        struct do_unary_op_value_inplace {
            typedef Range<typename dcT::iterator> rangeT;
            implT* impl;
            opT op;
            do_unary_op_value_inplace(implT* impl, const opT& op) : impl(impl), op(op) {}
            bool operator()(typename rangeT::iterator& it) const {
                const keyT& key = it->first;
                nodeT& node = it->second;
                if (node.has_coeff()) {
                    tensorT& t= node.coeff();
                    //double before = t.normf();
                    tensorT values = impl->fcube_for_mul(key, key, t);
                    op(key, values);
                    double scale = pow(0.5,0.5*NDIM*key.level())*sqrt(FunctionDefaults<NDIM>::get_cell_volume());
                    t = transform(values,impl->cdata.quad_phiw).scale(scale);
                    //double after = t.normf();
                    //madness::print("XOP:", key, before, after);
                }
                return true;
            }
            template <typename Archive> void serialize(const Archive& ar) {};
        };


        /// Unary operation applied inplace to the values with optional refinement and fence
        template <typename opT>
        void unary_op_value_inplace(const opT& op, bool fence)
        {
            typedef Range<typename dcT::iterator> rangeT;
            typedef do_unary_op_value_inplace<opT> xopT;
            world.taskq.for_each<rangeT,xopT>(rangeT(coeffs.begin(), coeffs.end()), xopT(this,op));
            if (fence) world.gop.fence();
        }


        // Multiplication assuming same distribution and recursive descent
        template <typename L, typename R>
        Void mulXXveca(const keyT& key,
                       const FunctionImpl<L,NDIM>* left, const Tensor<L>& lcin,
                       const std::vector<const FunctionImpl<R,NDIM>*> vrightin,
                       const std::vector< Tensor<R> >& vrcin,
                       const std::vector<FunctionImpl<T,NDIM>*> vresultin,
                       double tol)
        {
            typedef typename FunctionImpl<L,NDIM>::dcT::const_iterator literT;
            typedef typename FunctionImpl<R,NDIM>::dcT::const_iterator riterT;

            double lnorm = 1e99;
            Tensor<L> lc = lcin;
            if (lc.size == 0) {
                literT it = left->coeffs.find(key).get();
                MADNESS_ASSERT(it != left->coeffs.end());
                lnorm = it->second.get_norm_tree();
                if (it->second.has_coeff()) lc = it->second.coeff();
            }

            // Loop thru RHS functions seeing if anything can be multiplied
            std::vector<FunctionImpl<T,NDIM>*> vresult;
            std::vector<const FunctionImpl<R,NDIM>*> vright;
            std::vector< Tensor<R> > vrc;
            vresult.reserve(vrightin.size());
            vright.reserve(vrightin.size());
            vrc.reserve(vrightin.size());

            for (unsigned int i=0; i<vrightin.size(); i++) {
                FunctionImpl<T,NDIM>* result = vresultin[i];
                const FunctionImpl<R,NDIM>* right = vrightin[i];
                Tensor<R> rc = vrcin[i];
                double rnorm;
                if (rc.size == 0) {
                    riterT it = right->coeffs.find(key).get();
                    MADNESS_ASSERT(it != right->coeffs.end());
                    rnorm = it->second.get_norm_tree();
                    if (it->second.has_coeff()) rc = it->second.coeff();
                }
                else {
                    rnorm = rc.normf();
                }

                if (rc.size && lc.size) { // Yipee!
                    result->task(world.rank(), &implT:: template do_mul<L,R>, key, lc, std::make_pair(key,rc));
                }
                else if (tol && lnorm*rnorm < truncate_tol(tol, key)) {
                    result->coeffs.replace(key, nodeT(tensorT(cdata.vk),false)); // Zero leaf
                }
                else {
                    result->coeffs.replace(key, nodeT(tensorT(),true)); // Interior node
                    vresult.push_back(result);
                    vright.push_back(right);
                    vrc.push_back(rc);
                }
            }

            if (vresult.size()) {
                Tensor<L> lss;
                if (lc.size) {
                    Tensor<L> ld(cdata.v2k);
                    ld(cdata.s0) = lc(___);
                    lss = left->unfilter(ld);
                }

                std::vector< Tensor<R> > vrss(vresult.size());
                for (unsigned int i=0; i<vresult.size(); i++) {
                    if (vrc[i].size) {
                        Tensor<R> rd(cdata.v2k);
                        rd(cdata.s0) = vrc[i](___);
                        vrss[i] = vright[i]->unfilter(rd);
                    }
                }

                for (KeyChildIterator<NDIM> kit(key); kit; ++kit) {
                    const keyT& child = kit.key();
                    Tensor<L> ll;

                    std::vector<Slice> cp = child_patch(child);

                    if (lc.size) ll = copy(lss(cp));

                    std::vector< Tensor<R> > vv(vresult.size());
                    for (unsigned int i=0; i<vresult.size(); i++) {
                        if (vrc[i].size) vv[i] = copy(vrss[i](cp));
                    }

                    task(coeffs.owner(child), &implT:: template mulXXveca<L,R>, child, left, ll, vright, vv, vresult, tol);
                }
            }
            return None;
        }


        // Multiplication using recursive descent and assuming same distribution
        template <typename L, typename R>
        Void mulXXa(const keyT& key,
                    const FunctionImpl<L,NDIM>* left, const Tensor<L>& lcin,
                    const FunctionImpl<R,NDIM>* right,const Tensor<R>& rcin, 
                    double tol)  
        {
            typedef typename FunctionImpl<L,NDIM>::dcT::const_iterator literT;
            typedef typename FunctionImpl<R,NDIM>::dcT::const_iterator riterT;

            double lnorm=1e99, rnorm=1e99;

            Tensor<L> lc = lcin;
            if (lc.size == 0) {
                literT it = left->coeffs.find(key).get();
                MADNESS_ASSERT(it != left->coeffs.end());
                lnorm = it->second.get_norm_tree();
                if (it->second.has_coeff()) lc = it->second.coeff();
            }

            Tensor<R> rc = rcin;
            if (rc.size == 0) {
                riterT it = right->coeffs.find(key).get();
                MADNESS_ASSERT(it != right->coeffs.end());
                rnorm = it->second.get_norm_tree();
                if (it->second.has_coeff()) rc = it->second.coeff();
            }

            if (rc.size && lc.size) { // Yipee!
                do_mul<L,R>(key, lc, std::make_pair(key,rc));
                return None;
            }

            if (tol) {
                if (lc.size) lnorm = lc.normf(); // Otherwise got from norm tree above
                if (rc.size) rnorm = rc.normf();
                if (lnorm*rnorm < truncate_tol(tol, key)) {
                    coeffs.replace(key, nodeT(tensorT(cdata.vk),false)); // Zero leaf node
                    return None;
                }
            }
                    
            // Recur down
            coeffs.replace(key, nodeT(tensorT(),true)); // Interior node

            Tensor<L> lss;
            if (lc.size) {
                Tensor<L> ld(cdata.v2k);
                ld(cdata.s0) = lc(___);
                lss = left->unfilter(ld);
            }
            
            Tensor<R> rss;
            if (rc.size) {
                Tensor<R> rd(cdata.v2k);
                rd(cdata.s0) = rc(___);
                rss = right->unfilter(rd);
            }
            
            for (KeyChildIterator<NDIM> kit(key); kit; ++kit) {
                const keyT& child = kit.key();
                Tensor<L> ll;
                Tensor<R> rr;
                if (lc.size) ll = copy(lss(child_patch(child)));
                if (rc.size) rr = copy(rss(child_patch(child)));
                
                task(coeffs.owner(child), &implT:: template mulXXa<L,R>, child, left, ll, right, rr, tol);
            }

            return None;
        }


        template <typename L, typename R>
        void mulXX(const FunctionImpl<L,NDIM>* left, const FunctionImpl<R,NDIM>* right, double tol, bool fence) {
            if (world.rank() == coeffs.owner(cdata.key0))
                mulXXa(cdata.key0, left, Tensor<L>(), right, Tensor<R>(), tol);
            if (fence) world.gop.fence();

            //verify_tree();
        }

        template <typename L, typename R>
        void mulXXvec(const FunctionImpl<L,NDIM>* left,
                      const std::vector<const FunctionImpl<R,NDIM>*>& vright,
                      const std::vector<FunctionImpl<T,NDIM>*>& vresult,
                      double tol,
                      bool fence) {
            std::vector< Tensor<R> > vr(vright.size());
            if (world.rank() == coeffs.owner(cdata.key0))
                mulXXveca(cdata.key0, left, Tensor<L>(), vright, vr, vresult, tol);
            if (fence) world.gop.fence();
        }

        Future<double> get_norm_tree_recursive(const keyT& key) const;

        mutable long box_leaf[1000];
        mutable long box_interior[1000];

        // horrifically non-scalable
        Void put_in_box(ProcessID from, long nl, long ni) const {
            if (world.size() > 1000) throw "NO!";
            box_leaf[from] = nl;
            box_interior[from] = ni;
            return None;
        }


        /// Prints summary of data distribution
        void print_info() const {
            if (world.size() >= 1000) return;
            for (int i=0; i<world.size(); i++) box_leaf[i] = box_interior[i] == 0;
            world.gop.fence();
            long nleaf=0, ninterior=0;
            for (typename dcT::const_iterator it=coeffs.begin(); it!=coeffs.end(); ++it) {
                const nodeT& node = it->second;
                if (node.is_leaf()) nleaf++;
                else ninterior++;
            }
            send(0, &implT::put_in_box, world.rank(), nleaf, ninterior);
            world.gop.fence();
            if (world.rank() == 0) {
                for (int i=0; i<world.size(); i++) {
                    printf("load: %5d %8ld %8ld\n", i, box_leaf[i], box_interior[i]);
                }
            }
            world.gop.fence();
        }

        /// Verify tree is properly constructed ... global synchronization involved

        /// If an inconsistency is detected, prints a message describing the error and
        /// then throws a madness exception.
        ///
        /// This is a reasonably quick and scalable operation that is
        /// useful for debugging and paranoia.
        void verify_tree() const;

        /// Walk up the tree returning pair(key,node) for first node with coefficients

        /// Three possibilities.
        ///
        /// 1) The coeffs are present and returned with the key of the containing node.
        ///
        /// 2) The coeffs are further up the tree ... the request is forwarded up.
        ///
        /// 3) The coeffs are futher down the tree ... an empty tensor is returned.
        ///
        /// !! This routine is crying out for an optimization to
        /// manage the number of messages being sent ... presently
        /// each parent is fetched 2^(n*d) times where n is the no. of
        /// levels between the level of evaluation and the parent.
        /// Alternatively, reimplement multiply as a downward tree
        /// walk and just pass the parent down.  Slightly less
        /// parallelism but much less communication.
        Void sock_it_to_me(const keyT& key,
                           const RemoteReference< FutureImpl< std::pair<keyT,tensorT> > >& ref) const;

        /// Evaluate a cube/slice of points ... plotlo and plothi are already in simulation coordinates

        /// No communications
        Tensor<T> eval_plot_cube(const coordT& plotlo,
                                 const coordT& plothi,
                                 const std::vector<long>& npt) const;

        /// Evaluate the function at a point in \em simulation coordinates

        /// Only the invoking process will get the result via the
        /// remote reference to a future.  Active messages may be sent
        /// to other nodes.
        Void eval(const Vector<double,NDIM>& xin,
                  const keyT& keyin,
                  const typename Future<T>::remote_refT& ref);


        /// Computes norm of low/high-order polyn. coeffs for autorefinement test

        /// t is a k^d tensor.  In order to screen the autorefinement
        /// during multiplication compute the norms of
        /// ... lo ... the block of t for all polynomials of order < k/2
        /// ... hi ... the block of t for all polynomials of order >= k/2
        ///
        /// k=5   0,1,2,3,4     --> 0,1,2 ... 3,4
        /// k=6   0,1,2,3,4,5   --> 0,1,2 ... 3,4,5
        ///
        /// k=number of wavelets, so k=5 means max order is 4, so max exactly
        /// representable squarable polynomial is of order 2.
        void tnorm(const tensorT& t, double* lo, double* hi) const;

        // This invoked if node has not been autorefined
        Void do_square_inplace(const keyT& key);


        // This invoked if node has been autorefined
        Void do_square_inplace2(const keyT& parent, const keyT& child, const tensorT& parent_coeff);

        /// Always returns false (for when autorefine is not wanted)
        bool noautorefine(const keyT& key, const tensorT& t) const {return false;}

        /// Returns true if this block of coeffs needs autorefining
        bool autorefine_square_test(const keyT& key, const tensorT& t) const {
            double lo, hi;
            tnorm(t, &lo, &hi);
            double test = 2*lo*hi + hi*hi;
            //print("autoreftest",key,thresh,truncate_tol(thresh, key),lo,hi,test);
            return test > truncate_tol(thresh, key);
        }


        /// Pointwise squaring of function with optional global fence

        /// If not autorefining, local computation only if not fencing.
        /// If autorefining, may result in asynchronous communication.
        void square_inplace(bool fence);

        /// Differentiation of function with optional global fence

        /// Result of differentiating f is placed into this which will
        /// have the same process map, etc., as f
        void diff(const implT& f, int axis, bool fence);


        /// Returns key of neighbor enforcing BC

        /// Out of volume keys are mapped to enforce the BC as follows.
        ///   * Periodic BC map back into the volume and return the correct key
        ///   * Zero BC - returns invalid() to indicate out of volume
        keyT neighbor(const keyT& key, int axis, int step) const;


        /// Returns key of general neighbor enforcing BC

        /// Out of volume keys are mapped to enforce the BC as follows.
        ///   * Periodic BC map back into the volume and return the correct key
        ///   * Zero BC - returns invalid() to indicate out of volume
        keyT neighbor(const keyT& key, const keyT& disp) const;


        /// Called by diff to find key and coeffs of neighbor enforcing BC

        /// Should work for any (small) step but only tested for step=+/-1
        ///
        /// do_diff1 handles the adpative refinement.  If it needs to refine, it calls
        /// forward_do_diff1 to pass the task locally or remotely with high priority.
        /// Actual differentiation is performend by do_diff2.
        Future< std::pair<keyT,tensorT> > find_neighbor(const keyT& key, int axis, int step) const;

        /// Used by diff1 to forward calls to diff1 elsewhere

        /// We cannot send a not ready future to another process
        /// so we send an active message to schedule the remote task.
        ///
        /// Local tasks that might produce additional communication
        /// are scheduled with high priority and plain-old compute
        /// tasks get normal priority.  This is an attempt to get all
        /// of the communication and adaptive refinement happening
        /// in parallel to productive computation.
        Void forward_do_diff1(const implT* f, int axis, const keyT& key,
                      const std::pair<keyT,tensorT>& left,
                      const std::pair<keyT,tensorT>& center,
                              const std::pair<keyT,tensorT>& right);


        Void do_diff1(const implT* f, int axis, const keyT& key,
                      const std::pair<keyT,tensorT>& left,
                      const std::pair<keyT,tensorT>& center,
                      const std::pair<keyT,tensorT>& right);

        Void do_diff2(const implT* f, int axis, const keyT& key,
                      const std::pair<keyT,tensorT>& left,
                      const std::pair<keyT,tensorT>& center,
                      const std::pair<keyT,tensorT>& right);

        /// Permute the dimensions according to map
        void mapdim(const implT& f, const std::vector<long>& map, bool fence);


        T eval_cube(Level n, coordT x, const tensorT c) const;


        /// Transform sum coefficients at level n to sums+differences at level n-1

        /// Given scaling function coefficients s[n][l][i] and s[n][l+1][i]
        /// return the scaling function and wavelet coefficients at the
        /// coarser level.  I.e., decompose Vn using Vn = Vn-1 + Wn-1.
        /// \code
        /// s_i = sum(j) h0_ij*s0_j + h1_ij*s1_j
        /// d_i = sum(j) g0_ij*s0_j + g1_ij*s1_j
        //  \endcode
        /// Returns a new tensor and has no side effects.  Works for any
        /// number of dimensions.
        ///
        /// No communication involved.
        tensorT filter(const tensorT& s) const {
            tensorT r(cdata.v2k,false);
            tensorT w(cdata.v2k,false);
            return fast_transform(s,cdata.hgT,r,w);
            //return transform(s,cdata.hgT);
        }


        ///  Transform sums+differences at level n to sum coefficients at level n+1

        ///  Given scaling function and wavelet coefficients (s and d)
        ///  returns the scaling function coefficients at the next finer
        ///  level.  I.e., reconstruct Vn using Vn = Vn-1 + Wn-1.
        ///  \code
        ///  s0 = sum(j) h0_ji*s_j + g0_ji*d_j
        ///  s1 = sum(j) h1_ji*s_j + g1_ji*d_j
        ///  \endcode
        ///  Returns a new tensor and has no side effects
        ///
        ///  If (sonly) ... then ss is only the scaling function coeff (and
        ///  assume the d are zero).  Works for any number of dimensions.
        ///
        /// No communication involved.
        tensorT unfilter(const tensorT& s) const {
            tensorT r(cdata.v2k,false);
            tensorT w(cdata.v2k,false);
            return fast_transform(s,cdata.hg,r,w);
            //return transform(s, cdata.hg);
        }

        /// Projects old function into new basis (only in reconstructed form)
        void project(const implT& old, bool fence) {
            vector<Slice> s(NDIM,Slice(0,old.cdata.k-1));
            for (typename dcT::const_iterator it=old.coeffs.begin(); it!=old.coeffs.end(); ++it) {
                const keyT& key = it->first;
                const nodeT& node = it->second;
                if (node.has_coeff()) {
                    tensorT c(cdata.vk);
                    c(s) = node.coeff();
                    coeffs.replace(key,nodeT(c,false));
                }
                else {
                    coeffs.replace(key,nodeT(tensorT(),true));
                }
            }
            if (fence) world.gop.fence();
        }

        Void refine_op(const keyT& key) {
            // Must allow for someone already having autorefined the coeffs
            // and we get a write accessor just in case they are already executing
            typename dcT::accessor acc;
            MADNESS_ASSERT(coeffs.find(acc,key));
            nodeT& node = acc->second;
            if (node.has_coeff() && key.level() < max_refine_level && autorefine_square_test(key, node.coeff())) {
                tensorT d(cdata.v2k);
                d(cdata.s0) = node.coeff();
                d = unfilter(d);
                node.clear_coeff();
                node.set_has_children(true);
                for (KeyChildIterator<NDIM> kit(key); kit; ++kit) {
                    const keyT& child = kit.key();
                    tensorT ss = copy(d(child_patch(child)));
                    coeffs.replace(child,nodeT(ss,false));
                }
            }
            return None;
        }

        Void refine_spawn(const keyT& key) {
            nodeT& node = coeffs.find(key).get()->second;
            if (node.has_children()) {
                for (KeyChildIterator<NDIM> kit(key); kit; ++kit)
                    task(coeffs.owner(kit.key()), &implT::refine_spawn, kit.key(), TaskAttributes::hipri());
            }
            else {
                task(coeffs.owner(key), &implT::refine_op, key);
            }
            return None;
        }

        // This needed extending to accomodate a user-defined criterion
        void refine(bool fence) {
            if (world.rank() == coeffs.owner(cdata.key0))
                task(coeffs.owner(cdata.key0), &implT::refine_spawn, cdata.key0, TaskAttributes::hipri());
            if (fence) world.gop.fence();
        }


        void reconstruct(bool fence) {
            // Must set true here so that successive calls without fence do the right thing
            nonstandard = compressed = false;
            if (world.rank() == coeffs.owner(cdata.key0))
                task(world.rank(), &implT::reconstruct_op, cdata.key0,tensorT());
            if (fence) world.gop.fence();
        }

        // Invoked on node where key is local
        Void reconstruct_op(const keyT& key, const tensorT& s);

        void compress(bool nonstandard, bool keepleaves, bool fence) {
            // Must set true here so that successive calls without fence do the right thing
            this->compressed = true;
            this->nonstandard = nonstandard;
            if (world.rank() == coeffs.owner(cdata.key0)) compress_spawn(cdata.key0, nonstandard, keepleaves);
            if (fence) world.gop.fence();
        }


        // Invoked on node where key is local
        Future<tensorT> compress_spawn(const keyT& key, bool nonstandard, bool keepleaves);

        void norm_tree(bool fence) {
           if (world.rank() == coeffs.owner(cdata.key0)) norm_tree_spawn(cdata.key0);
           if (fence) world.gop.fence();
        }

        double norm_tree_op(const keyT& key, const vector< Future<double> >& v) {
            PROFILE_MEMBER_FUNC(FunctionImpl);
            double sum = 0.0;
            int i=0;
            for (KeyChildIterator<NDIM> kit(key); kit; ++kit,++i) {
                double value = v[i].get();
                sum += value*value;
            }
            sum = sqrt(sum);
            coeffs.send(key, &nodeT::set_norm_tree, sum);
            //if (key.level() == 0) std::cout << "NORM_TREE_TOP " << sum << "\n";
            return sum;
        }

        Future<double> norm_tree_spawn(const keyT& key) {
            nodeT& node = coeffs.find(key).get()->second;
            if (node.has_children()) {
                std::vector< Future<double> > v = future_vector_factory<double>(1<<NDIM);
                int i=0;
                for (KeyChildIterator<NDIM> kit(key); kit; ++kit,++i) {
                    v[i] = task(coeffs.owner(kit.key()), &implT::norm_tree_spawn, kit.key());
                }
                return task(world.rank(),&implT::norm_tree_op, key, v);
            }
            else {
                return Future<double>(node.coeff().normf());
            }
        }

        tensorT compress_op(const keyT& key, const vector< Future<tensorT> >& v, bool nonstandard) {
            PROFILE_MEMBER_FUNC(FunctionImpl);
            // Copy child scaling coeffs into contiguous block
            tensorT d(cdata.v2k,false);
            int i=0;
            for (KeyChildIterator<NDIM> kit(key); kit; ++kit,++i) {
                d(child_patch(kit.key())) = v[i].get();
            }
            d = filter(d);
            tensorT s = copy(d(cdata.s0));
            if (key.level() > 0 && !nonstandard) d(cdata.s0) = 0.0;
            coeffs.replace(key, nodeT(d,true));
            return s;
        }

        /// Changes non-standard compressed form to standard compressed form
        void standard(bool fence) {
            for (typename dcT::iterator it=coeffs.begin(); it!=coeffs.end(); ++it) {
                const keyT& key = it->first;
                nodeT& node = it->second;
                if (key.level() > 0 && node.has_coeff()) {
                    if (node.has_children()) {
                        // Zero out scaling coeffs
                        node.coeff()(cdata.s0) = 0.0;
                    }
                    else {
                        // Deleting both scaling and wavelet coeffs
                        node.clear_coeff();
                    }
                }
            }
            if (fence) world.gop.fence();
        }

        struct do_op_args {
            keyT key, d, dest;
            double tol, fac, cnorm;
            do_op_args() {}
            do_op_args(const keyT& key, const keyT& d, const keyT& dest, double tol, double fac, double cnorm)
                : key(key), d(d), dest(dest), tol(tol), fac(fac), cnorm(cnorm) {}
            template <class Archive>
            void serialize(Archive& ar) {ar & archive::wrap_opaque(this,1);}
        };

        template <typename opT, typename R>
        Void do_apply_kernel(const opT* op, const Tensor<R>& c, const do_op_args& args) {
            tensorT result = op->apply(args.key, args.d, c, args.tol/args.fac/args.cnorm);

            //print("APPLY", key, d, opnorm, cnorm, result.normf());

            // Screen here to reduce communication cost of negligible data
            // and also to ensure we don't needlessly widen the tree when
            // applying the operator
            if (result.normf() > 0.3*args.tol/args.fac) {
                coeffs.send(args.dest, &nodeT::accumulate, result, coeffs, args.dest);
            }
            return None;
        }

        template <typename opT, typename R>
        Void do_apply(const opT* op, const FunctionImpl<R,NDIM>* f, const keyT& key, const Tensor<R>& c) {
            PROFILE_MEMBER_FUNC(FunctionImpl);
	    // insert timer here
	    double start_time = 0;// cpu_time();
	    double end_time = 0, cum_time = 0;
            double fac = 3.0; // 10.0 seems good for qmprop ... 3.0 OK for others
            double cnorm = c.normf();
	    const long lmax = 1L << (key.level()-1);
	    start_time = cpu_time();
            const std::vector<keyT>& disp = op->get_disp(key.level());
            for (typename std::vector<keyT>::const_iterator it=disp.begin();  it != disp.end(); ++it) {
                const keyT& d = *it;

                keyT dest = neighbor(key, d);

                // For periodic directions restrict translations to be no more than
                // half of the unit cell to avoid double counting.
                bool doit = true;
                for (int i=0; i<NDIM; i++) {
                    if (bc(i,0) == 1) {
                        if (d.translation()[i] > lmax || d.translation()[i] <= -lmax) doit = false;
                        break;
                    }
                }
                if (!doit) break;


                if (dest.is_valid()) {
                    double opnorm = op->norm(key.level(), d);
                    // working assumption here is that the operator is isotropic and
                    // montonically decreasing with distance
                    double tol = truncate_tol(thresh, key);

                    if (cnorm*opnorm > tol/fac) {
                        do_op_args args(key, d, dest, tol, fac, cnorm);
                        task(world.rank(), &implT:: template do_apply_kernel<opT,R>, op, c, args);
                        //task(coeffs.owner(dest), &implT:: template do_apply_kernel<opT,R>, op, c, args);
                        //task(world.random_proc(), &implT:: template do_apply_kernel<opT,R>, op, c, args);
                    }
                    else if (d.distsq() >= 1) { // Assumes monotonic decay beyond nearest neighbor
                        break;
                    }
                }

             }
	    // update Apply_Time
	    end_time = cpu_time();
	    //	    madness::print("time for key", key, ":", end_time-start_time);
	    if (apply_time) {
	      cum_time = end_time - start_time;
	      apply_time->update(key, cum_time);
	    }
            return None;
        }

        template <typename opT, typename R>
        void apply(opT& op, const FunctionImpl<R,NDIM>& f, bool fence) {
            PROFILE_MEMBER_FUNC(FunctionImpl);
            for (typename dcT::const_iterator it=f.coeffs.begin(); it!=f.coeffs.end(); ++it) {
                const keyT& key = it->first;
                const FunctionNode<R,NDIM>& node = it->second;
                if (node.has_coeff()) {
                    if (node.coeff().dim[0] != k || op.doleaves) {
                        ProcessID p;
                        if (FunctionDefaults<NDIM>::get_apply_randomize()) {
                            p = world.random_proc();
                        }
                        else {
                            p = coeffs.owner(key);
                        }
                        task(p, &implT:: template do_apply<opT,R>, &op, &f, key, node.coeff());
                    }
                }
            }
            if (fence) world.gop.fence();
        }

      /// accessor functions for apply_time
      // no good place to put them, so here seems as good as any
//       double get_apply_time(const keyT& key) {
// 	return apply_time.get(key);
//       }

//       void print_apply_time() {
// 	apply_time.print();
//       }

        void set_apply_time_ptr(SharedPtr<ApplyTime<NDIM> > ptr) {
            apply_time = ptr;
        }


        /// Returns the square of the error norm in the box labelled by key

        /// Assumed to be invoked locally but it would be easy to eliminate
        /// this assumption
        template <typename opT>
        double err_box(const keyT& key, const nodeT& node, const opT& func,
                       int npt, const Tensor<double>& qx, const Tensor<double>& quad_phit,
                       const Tensor<double>& quad_phiw) const {

            std::vector<long> vq(NDIM);
            for (int i=0; i<NDIM; i++) vq[i] = npt;
            tensorT fval(vq,false), work(vq,false), result(vq,false);

            // Compute the "exact" function in this volume at npt points
            // where npt is usually this->npt+1.
            fcube(key, func, qx, fval);

            // Transform into the scaling function basis of order npt
            double scale = pow(0.5,0.5*NDIM*key.level())*sqrt(FunctionDefaults<NDIM>::get_cell_volume());
            fval = fast_transform(fval,quad_phiw,result,work).scale(scale);

            // Subtract to get the error ... the original coeffs are in the order k
            // basis but we just computed the coeffs in the order npt(=k+1) basis
            // so we can either use slices or an iterator macro.
            const tensorT& coeff = node.coeff();
            ITERATOR(coeff,fval(IND)-=coeff(IND););

            // Compute the norm of what remains
            double err = fval.normf();
            return err*err;
        }

        template <typename opT>
        class do_err_box {
            const implT* impl;
            const opT* func;
            int npt;
            Tensor<double> qx;
            Tensor<double> quad_phit;
            Tensor<double> quad_phiw;
        public:
            do_err_box() {}

            do_err_box(const implT* impl, const opT* func, int npt, const Tensor<double>& qx,
                       const Tensor<double>& quad_phit, const Tensor<double>& quad_phiw)
                : impl(impl), func(func), npt(npt), qx(qx), quad_phit(quad_phit), quad_phiw(quad_phiw)
            {}

            do_err_box(const do_err_box& e)
                : impl(e.impl), func(e.func), npt(e.npt), qx(e.qx), quad_phit(e.quad_phit), quad_phiw(e.quad_phiw)
            {}

            double operator()(typename dcT::const_iterator& it) const {
                const keyT& key = it->first;
                const nodeT& node = it->second;
                if (node.has_coeff()) return impl->err_box(key, node, *func, npt, qx, quad_phit, quad_phiw);
                else return 0.0;
            }

            double operator()(double a, double b) const {return a+b;}

            template <typename Archive>
            void serialize(const Archive& ar) {
                throw "not yet";
            }
        };


        /// Returns the sum of squares of errors from local info ... no comms
        template <typename opT>
        double errsq_local(const opT& func) const {
            PROFILE_MEMBER_FUNC(FunctionImpl);
            // Make quadrature rule of higher order
            const int npt = cdata.npt + 1;
            Tensor<double> qx, qw, quad_phi, quad_phiw, quad_phit;
            FunctionCommonData<T,NDIM>::_init_quadrature(k+1, npt, qx, qw, quad_phi, quad_phiw, quad_phit);

            typedef Range<typename dcT::const_iterator> rangeT;
            rangeT range(coeffs.begin(), coeffs.end());
            return world.taskq.reduce< double,rangeT,do_err_box<opT> >(range,
                                                                       do_err_box<opT>(this, &func, npt, qx, quad_phit, quad_phiw));
        }


        /// Returns \c int(f(x),x) in local volume
        T trace_local() const;


        struct do_norm2sq_local {
            double operator()(typename dcT::const_iterator& it) const {
                const nodeT& node = it->second;
                if (node.has_coeff()) {
                    double norm = node.coeff().normf();
                    return norm*norm;
                }
                else {
                    return 0.0;
                }
            }

            double operator()(double a, double b) const {return a+b;}

            template <typename Archive> void serialize(const Archive& ar){};
        };


        /// Returns the square of the local norm ... no comms
        double norm2sq_local() const {
            PROFILE_MEMBER_FUNC(FunctionImpl);
            typedef Range<typename dcT::const_iterator> rangeT;
            return world.taskq.reduce<double,rangeT,do_norm2sq_local>(rangeT(coeffs.begin(),coeffs.end()),
                                                                      do_norm2sq_local());
        }

	/// Returns the inner product ASSUMING same distribution
	template <typename R>
	  TENSOR_RESULT_TYPE(T,R) inner_local(const FunctionImpl<R,NDIM>& g) const {

	  TENSOR_RESULT_TYPE(T,R) sum = 0.0;
	  for (typename dcT::const_iterator it=coeffs.begin(); it!=coeffs.end(); ++it) {
	    const nodeT& fnode = it->second;
	    if (fnode.has_coeff()) {
	      if (g.coeffs.probe(it->first)) {
		const FunctionNode<R,NDIM>& gnode = g.coeffs.find(it->first).get()->second;
		if (gnode.has_coeff()) {
                  if (gnode.coeff().dim[0] != fnode.coeff().dim[0]) {
			madness::print("INNER", it->first, gnode.coeff().dim[0],fnode.coeff().dim[0]);
                        throw "adios";
                  }
		  sum += fnode.coeff().trace_conj(gnode.coeff());
		}
	      }
            }
	  }
	  return sum;
	}

        /// Returns the maximum depth of the tree
	std::size_t max_depth() const {
	    std::size_t maxdepth = 0;
	    for (typename dcT::const_iterator it=coeffs.begin(); it!=coeffs.end(); ++it) {
		std::size_t N = (std::size_t) it->first.level();
		if (N > maxdepth) maxdepth = N;
	    }
	    world.gop.max(maxdepth);
	    return maxdepth;
	}

        /// Returns the max number of nodes on a processor
	std::size_t max_nodes() const {
	    std::size_t maxsize = 0;
	    maxsize = coeffs.size();
	    world.gop.max(maxsize);
	    return maxsize;
	}

        /// Returns the min number of nodes on a processor
	std::size_t min_nodes() const {
	    std::size_t minsize = 0;
	    minsize = coeffs.size();
	    world.gop.min(minsize);
	    return minsize;
	}


        /// Returns the size of the tree structure of the function ... collective global sum
	std::size_t tree_size() const {
	    std::size_t sum = 0;
	    sum = coeffs.size();
	    world.gop.sum(sum);
	    return sum;
	}

        /// Returns the number of coefficients in the function ... collective global sum
        std::size_t size() const {
            std::size_t sum = 0;
            for (typename dcT::const_iterator it=coeffs.begin(); it!=coeffs.end(); ++it) {
                const nodeT& node = it->second;
                if (node.has_coeff()) sum++;
            }
            if (is_compressed()) for (int i=0; i<NDIM; i++) sum *= 2*cdata.k;
            else                 for (int i=0; i<NDIM; i++) sum *=   cdata.k;

            world.gop.sum(sum);

            return sum;
        }

        /// In-place scale by a constant
        void scale_inplace(const T q, bool fence);

        /// Out-of-place scale by a constant
        template <typename Q, typename F>
        void scale_oop(const Q q, const FunctionImpl<F,NDIM>& f, bool fence) {
            typedef typename FunctionImpl<F,NDIM>::nodeT fnodeT;
            typedef typename FunctionImpl<F,NDIM>::dcT fdcT;
            for (typename fdcT::const_iterator it=f.coeffs.begin(); it!=f.coeffs.end(); ++it) {
                const keyT& key = it->first;
                const fnodeT& node = it->second;
                if (node.has_coeff()) {
                    coeffs.replace(key,nodeT(node.coeff()*q,node.has_children()));
                }
                else {
                    coeffs.replace(key,nodeT(tensorT(),node.has_children()));
                }
            }
            if (fence) world.gop.fence();
        }


    private:
        /// Assignment is not allowed ... not even possible now that we have reference members
        //FunctionImpl<T>& operator=(const FunctionImpl<T>& other);
    };

    namespace archive {
        template <class Archive, class T, int NDIM>
        struct ArchiveLoadImpl<Archive,const FunctionImpl<T,NDIM>*> {
            static void load(const Archive& ar, const FunctionImpl<T,NDIM>*& ptr) {
                uniqueidT id;
                ar & id;
                World* world = World::world_from_id(id.get_world_id());
                MADNESS_ASSERT(world);
                ptr = static_cast< const FunctionImpl<T,NDIM>* >(world->ptr_from_id< WorldObject< FunctionImpl<T,NDIM> > >(id));
                if (!ptr) MADNESS_EXCEPTION("FunctionImpl: remote operation attempting to use a locally uninitialized object",0);
            }
        };

        template <class Archive, class T, int NDIM>
        struct ArchiveStoreImpl<Archive,const FunctionImpl<T,NDIM>*> {
            static void store(const Archive& ar, const FunctionImpl<T,NDIM>*const& ptr) {
                ar & ptr->id();
            }
        };

        template <class Archive, class T, int NDIM>
        struct ArchiveLoadImpl<Archive, FunctionImpl<T,NDIM>*> {
            static void load(const Archive& ar, FunctionImpl<T,NDIM>*& ptr) {
                uniqueidT id;
                ar & id;
                World* world = World::world_from_id(id.get_world_id());
                MADNESS_ASSERT(world);
                ptr = static_cast< FunctionImpl<T,NDIM>* >(world->ptr_from_id< WorldObject< FunctionImpl<T,NDIM> > >(id));
                if (!ptr) MADNESS_EXCEPTION("FunctionImpl: remote operation attempting to use a locally uninitialized object",0);
            }
        };

        template <class Archive, class T, int NDIM>
        struct ArchiveStoreImpl<Archive, FunctionImpl<T,NDIM>*> {
            static void store(const Archive& ar, FunctionImpl<T,NDIM>*const& ptr) {
                ar & ptr->id();
            }
        };
    }



}

#endif
