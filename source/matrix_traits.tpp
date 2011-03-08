/*
 * matrix_traits.tpp
 * Provides compile-time inspection
 *
 *  Created on: 6 Aug 2010
 *      Author: alexf
 */

#pragma once

#include <VW/Image/imagergb.h>
#include <VW/Image/imagemono.h>
#include <VW/Image/imagecopy.tpp>

#include "common_types.h"

#include "vw_image.tpp"

namespace indoor_context {
	// matrix_width
	template<typename T>
	int matrix_width(const toon::Matrix<toon::Dynamic, toon::Dynamic, T>& A) {
		return A.num_cols();
	}
	template<int M, int N, typename T, typename Layout>
	int matrix_width(const toon::Matrix<M,N,T,Layout>& A) {
		return N;
	}
	template<int M, int N, typename T>
	int matrix_width(const VNL::MatrixFixed<M,N,T>& A) {
		return N;
	}
	template<typename T>
	int matrix_width(const VNL::Matrix<T>& A) {
		return A.Cols();
	}
	template<typename T>
	int matrix_width(const T& A) {
		return A.nx();
	}


	// matrix_height
	template<typename T>
	int matrix_height(const toon::Matrix<toon::Dynamic, toon::Dynamic, T>& A) {
		return A.num_rows();
	}
	template<int M, int N, typename T, typename Layout>
	int matrix_height(const toon::Matrix<M,N,T,Layout>& A) {
		return M;
	}
	template<int M, int N, typename T>
	int matrix_height(const VNL::MatrixFixed<M,N,T>& x) {
		return M;
	}
	template<typename T>
	int matrix_height(const VNL::Matrix<T>& A) {
		return A.Rows();
	}
	template<typename T>
	int matrix_height(const T& A) {
		return A.ny();
	}
	




	// matrix_traits implementation for images
	template<typename T, typename S>
	inline int matrix_width(const VW::ImageBase<T,S>& image) {
		return image.GetWidth();
	}
	template<typename T, typename S>
	inline int matrix_height(const VW::ImageBase<T,S>& image) {
		return image.GetHeight();
	}
	template<typename T>
	inline int matrix_width(const VW::ImageMono<T>& image) {
		return image.GetWidth();
	}
	template<typename T>
	inline int matrix_height(const VW::ImageMono<T>& image) {
		return image.GetHeight();
	}
	template<typename T>
	inline int matrix_width(const VW::ImageRGB<T>& image) {
		return image.GetWidth();
	}
	template<typename T>
	inline int matrix_height(const VW::ImageRGB<T>& image) {
		return image.GetHeight();
	}

	// Hack because the above weren't being picked up for some bizarre reason...
	template<>
	inline int matrix_width(const VW::ImageMono<float>& image) {
		return image.GetWidth();
	}
	template<>
	inline int matrix_height(const VW::ImageMono<float>& image) {
		return image.GetHeight();
	}
	template<>
	inline int matrix_width(const VW::ImageRGB<byte>& image) {
		return image.GetWidth();
	}
	template<>
	inline int matrix_height(const VW::ImageRGB<byte>& image) {
		return image.GetHeight();
	}



	// matrix_size
	template <typename T>
	Vec2I matrix_size(const T& A) {
		return toon::makeVector(matrix_width(A), matrix_height(A));
	}


	template <typename T>
	struct matrix_traits { };

	template <int Rows, int Cols, class Precision, class Layout>
	struct matrix_traits<toon::Matrix<Rows,Cols,Precision,Layout> > {
		typedef typename toon::Matrix<Rows,Cols,Precision,Layout> matrix_type;
		typedef Precision value_type;
		typedef Layout layout;
		static const int fixed_rows = Rows;
		static const int fixed_cols = Cols;
		static const bool is_fixed_size = true;
	};

	template <class Precision, class Layout>
	struct matrix_traits<toon::Matrix<-1,-1,Precision,Layout> > {
		typedef typename toon::Matrix<-1,-1,Precision,Layout> matrix_type;
		typedef Precision value_type;
		typedef Layout layout;
		static const bool is_fixed_size = false;
	};

	template <class Precision>
	struct matrix_traits<VNL::Matrix<Precision> > {
		typedef typename VNL::Matrix<Precision> matrix_type;
		typedef Precision value_type;
		static const bool is_fixed_size = false;
	};

}  // namespace indoor_context
