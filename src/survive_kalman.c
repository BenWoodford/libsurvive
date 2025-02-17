#include "survive_kalman.h"
#if !defined(__FreeBSD__) && !defined(__APPLE__)
#include <malloc.h>
#endif
#include <memory.h>
#include <sv_matrix.h>

#include "math.h"

typedef struct SvMat survive_kalman_gain_matrix;

#define KALMAN_LOG_LEVEL 1000

#define SV_KALMAN_VERBOSE(lvl, fmt, ...)                                                                               \
	{                                                                                                                  \
		if (k->log_level >= lvl) {                                                                                     \
			fprintf(stdout, fmt "\n", __VA_ARGS__);                                                                    \
		}                                                                                                              \
	}

void survive_kalman_set_logging_level(survive_kalman_state_t *k, int v) { k->log_level = v; }

static void sv_print_mat_v(const survive_kalman_state_t *k, int ll, const char *name, const SvMat *M, bool newlines) {
	if (k->log_level < ll) {
		return;
	}
	char term = newlines ? '\n' : ' ';
	if (!M) {
		fprintf(stdout, "null%c", term);
		return;
	}
	fprintf(stdout, "%4s %2d x %2d:%c", name, M->rows, M->cols, term);
	FLT scale = sv_sum(M);
	for (unsigned i = 0; i < M->rows; i++) {
		for (unsigned j = 0; j < M->cols; j++) {
			FLT v = svMatrixGet(M, i, j);
			if (v == 0)
				fprintf(stdout, "         0,\t");
			else
				fprintf(stdout, "%+5.2e,\t", v);
		}
		if (newlines)
			fprintf(stdout, "\n");
	}
	fprintf(stdout, "\n");
}
static void sv_print_mat(survive_kalman_state_t *k, const char *name, const SvMat *M, bool newlines) {
	sv_print_mat_v(k, KALMAN_LOG_LEVEL, name, M, newlines);
}

static void kalman_linear_predict(FLT t, const survive_kalman_state_t *k, const SvMat *x_t0_t0, SvMat *x_t0_t1) {
	int state_cnt = k->state_cnt;
	SV_CREATE_STACK_MAT(F, state_cnt, state_cnt);
	k->F_fn(t, &F, x_t0_t0);

	// X_k|k-1 = F * X_k-1|k-1
	svGEMM(&F, x_t0_t0, 1, 0, 0, x_t0_t1, 0);
	SV_FREE_STACK_MAT(F);
}

void user_is_q(void *user, FLT t, const struct SvMat *x, SvMat *Q_out) {
	const SvMat *q = (const SvMat *)user;
	scalend(SV_FLT_PTR(Q_out), SV_FLT_PTR(q), t, x->rows * x->rows);
}

SURVIVE_EXPORT void survive_kalman_state_reset(survive_kalman_state_t *k) {
	k->t = 0;
	sv_set_zero(&k->P);

	k->Q_fn(k->user, 1, &k->state, &k->P);
	sv_print_mat(k, "initial Pk_k", &k->P, true);
}

static void transition_is_identity(FLT t, struct SvMat *f_out, const struct SvMat *x0) {
	sv_eye(f_out, 0);
}

void survive_kalman_state_init(survive_kalman_state_t *k, size_t state_cnt, kalman_transition_fn_t F,
							   kalman_process_noise_fn_t q_fn, void *user, FLT *state) {
	memset(k, 0, sizeof(*k));

	k->state_cnt = (int)state_cnt;
	k->F_fn = F ? F : transition_is_identity;
	k->Q_fn = q_fn ? q_fn : user_is_q;

	k->P = svMat(k->state_cnt, k->state_cnt, 0);

	k->Predict_fn = kalman_linear_predict;
	k->user = user;

	if (!state) {
		k->State_is_heap = true;
		state = SV_CALLOC(sizeof(FLT) * k->state_cnt);
	}

	k->state = svMat(k->state_cnt, 1, state);
}

void survive_kalman_state_free(survive_kalman_state_t *k) {
	free(k->P.data);
	k->P.data = 0;

	if (k->State_is_heap)
		free(SV_FLT_PTR(&k->state));
	k->state.data = 0;
}

void survive_kalman_predict_covariance(FLT t, const SvMat *F, const SvMat *x, survive_kalman_state_t *k) {
	int dims = k->state_cnt;

	SvMat *Pk1_k1 = &k->P;
	sv_print_mat(k, "Pk-1_k-1", Pk1_k1, 1);
	SV_CREATE_STACK_MAT(Q, dims, dims);
	k->Q_fn(k->user, t, x, &Q);

	// k->P = F * k->P * F^T + Q
	matrix_ABAt_add(Pk1_k1, F, Pk1_k1, &Q);

	if (k->log_level >= KALMAN_LOG_LEVEL) {
		SV_KALMAN_VERBOSE(110, "T: %f", t);
		sv_print_mat(k, "Q", &Q, 1);
		sv_print_mat(k, "F", F, 1);
		sv_print_mat(k, "Pk1_k-1", Pk1_k1, 1);
	}
	SV_FREE_STACK_MAT(Q);
}

static void survive_kalman_update_covariance(survive_kalman_state_t *k, survive_kalman_gain_matrix *K,
											 const struct SvMat *H, const SvMat *R) {
	int dims = k->state_cnt;

	SvMat *Pk_k = &k->P;

	SV_CREATE_STACK_MAT(Pk_k1Ht, dims, H->rows);

	// Pk_k1Ht = P_k|k-1 * H^T
	svGEMM(Pk_k, H, 1, 0, 0, &Pk_k1Ht, SV_GEMM_FLAG_B_T);
	SV_CREATE_STACK_MAT(S, H->rows, H->rows);

	sv_print_mat(k, "H", H, 1);
	sv_print_mat(k, "R", R, 1);

	// S = H * P_k|k-1 * H^T + R
	svGEMM(H, &Pk_k1Ht, 1, R, 1, &S, 0);

	sv_print_mat(k, "Pk_k1Ht", &Pk_k1Ht, 1);
	sv_print_mat(k, "S", &S, 1);

	SV_CREATE_STACK_MAT(iS, H->rows, H->rows);

	FLT diag = 0, non_diag = 0;
	for (int i = 0; i < H->rows; i++) {
		for (int j = 0; j < H->rows; j++) {
			if (i == j) {
				diag += fabs(_S[i + j * H->rows]);
				_iS[i + j * H->rows] = 1. / _S[i + j * H->rows];
			} else {
				non_diag += fabs(_S[i + j * H->rows]);
				_iS[i + j * H->rows] = 0;
			}
		}
	}

	if (diag == 0 || non_diag / diag > 1e-5) {
		svInvert(&S, &iS, SV_INVERT_METHOD_LU);
	}
	sv_print_mat(k, "iS", &iS, 1);

	// K = Pk_k1Ht * iS
	svGEMM(&Pk_k1Ht, &iS, 1, 0, 0, K, 0);

	// Apparently cvEye isn't a thing!?
	SV_CREATE_STACK_MAT(eye, dims, dims);
	sv_set_diag_val(&eye, 1);

	SV_CREATE_STACK_MAT(ikh, dims, dims);

	// ikh = (I - K * H)
	svGEMM(K, H, -1, &eye, 1, &ikh, 0);

	// cvGEMM does not like the same addresses for src and destination...
	SV_CREATE_STACK_MAT(tmp, dims, dims);
	svCopy(Pk_k, &tmp, 0);

	// P_k|k = (I - K * H) * P_k|k-1
	svGEMM(&ikh, &tmp, 1, 0, 0, Pk_k, 0);

	if (k->log_level >= KALMAN_LOG_LEVEL) {
		fprintf(stdout, "INFO gain\t");
		sv_print_mat(k, "K", K, true);

		sv_print_mat(k, "ikh", &ikh, true);

		fprintf(stdout, "INFO new Pk_k\t");
		sv_print_mat(k, "Pk_k", Pk_k, true);
	}
	SV_FREE_STACK_MAT(tmp);
	SV_FREE_STACK_MAT(ikh);
	SV_FREE_STACK_MAT(eye);
	SV_FREE_STACK_MAT(iS);
	SV_FREE_STACK_MAT(S);
	SV_FREE_STACK_MAT(Pk_k1Ht);
}

static inline void survive_kalman_predict(FLT t, survive_kalman_state_t *k, const SvMat *x_t0_t0, SvMat *x_t0_t1) {
	// X_k|k-1 = Predict(X_K-1|k-1)
	if (k->log_level > KALMAN_LOG_LEVEL) {
		fprintf(stdout, "INFO kalman_predict from ");
		sv_print_mat(k, "x_t0_t0", x_t0_t0, false);
	}
	assert(sv_as_const_vector(x_t0_t0) != sv_as_const_vector(x_t0_t1));
	if (t == k->t) {
		svCopy(x_t0_t0, x_t0_t1, 0);
	} else {
		assert(t > k->t);
		k->Predict_fn(t - k->t, k, x_t0_t0, x_t0_t1);
	}
	if (k->log_level > KALMAN_LOG_LEVEL) {
		fprintf(stdout, "INFO kalman_predict to   ");
		sv_print_mat(k, "x_t0_t1", x_t0_t1, false);
	}
	if (k->datalog) {
		SV_CREATE_STACK_MAT(tmp, x_t0_t0->rows, x_t0_t1->cols);
		sv_elementwise_subtract(&tmp, x_t0_t1, x_t0_t0);
		k->datalog(k, "predict_diff", sv_as_const_vector(&tmp), tmp.rows * tmp.cols);
	}
}

static void linear_update(FLT dt, survive_kalman_state_t *k, const SvMat *y, const SvMat *K, const SvMat *x_t0,
						  SvMat *x_t1) {
	if (k->log_level > KALMAN_LOG_LEVEL || k->datalog) {
		if (k->log_level > KALMAN_LOG_LEVEL) {
			fprintf(stdout, "INFO linear_update dt=%f\n", dt);
		}
		sv_print_mat(k, "y", y, false);

		SV_CREATE_STACK_MAT(tmp, x_t1->rows, x_t1->cols);
		svGEMM(K, y, 1, 0, 0, &tmp, 0);
		sv_print_mat(k, "K*y", &tmp, false);
		if (k->datalog) {
			k->datalog(k, "ky_append", sv_as_const_vector(&tmp), tmp.rows);
		}
		SV_FREE_STACK_MAT(tmp);
	}

	assert(x_t0 != x_t1);
	// X_k|k = X_k|k-1 + K * y
	svGEMM(K, y, 1, x_t0, 1, x_t1, 0);
}

static SvMat *survive_kalman_find_residual(FLT dt, survive_kalman_state_t *k, kalman_measurement_model_fn_t Hfn,
										   void *user, const struct SvMat *Z, const struct SvMat *x, SvMat *y,
										   SvMat *H) {
	sv_set_constant(H, INFINITY);

	SvMat *rtn = 0;
	if (Hfn) {
		// typedef void (*kalman_measurement_model_fn_t)(void* user, FLT t, const struct SvMat * Z, const struct SvMat
		// *x_t, struct SvMat* h_x_t, struct SvMat* H_k);
		bool okay = Hfn(user, Z, x, y, H);
		if (okay == false) {
			return 0;
		}
		sv_print_mat_v(k, 600, "Hk", H, true);
		rtn = H;
	} else {
		rtn = (struct SvMat *)user;
		svGEMM(rtn, x, -1, Z, 1, y, 0);
	}
	assert(sv_is_finite(rtn));

	return rtn;
}

static FLT survive_kalman_predict_update_state_extended_adaptive_internal(FLT t, survive_kalman_state_t *k,
																		  const struct SvMat *Z, FLT *Rv,
																		  kalman_measurement_model_fn_t Hfn, void *user,
																		  bool adaptive) {
	int state_cnt = k->state_cnt;
	struct SvMat *H = 0;
	FLT dt = t - k->t;
	FLT result = 0;

	// Anything coming in this soon is liable to spike stuff since dt is so small
	if (dt < 1e-5) {
		dt = 0;
		t = k->t;
	}

	SV_CREATE_STACK_MAT(Pm, state_cnt, state_cnt);
	// Adaptive update happens on the covariance matrix prior; so save it.
	if (adaptive)
		sv_matrix_copy(&Pm, &k->P);

	SV_CREATE_STACK_MAT(y, Z->rows, Z->cols);

	// To avoid an unneeded copy, x1 here is both X_k-1|k-1 and X_k|k.
	// x is X_k|k-1
	SvMat *x1 = &k->state;

	// Predict x
	SV_CREATE_STACK_MAT(x2, state_cnt, 1);
	survive_kalman_predict(t, k, x1, &x2);

	SV_CREATE_STACK_MAT(HStorage, Z->rows, state_cnt);
	H = survive_kalman_find_residual(dt, k, Hfn, user, Z, &x2, &y, &HStorage);

	if (k->datalog) {
		k->datalog(k, "Z", sv_as_const_vector(Z), Z->rows);
		k->datalog(k, "y", sv_as_const_vector(&y), y.rows);
	}

	if (k->log_level > KALMAN_LOG_LEVEL) {
		fprintf(stdout, "INFO kalman_predict_update_state_extended t=%f dt=%f ", t, dt);
		sv_print_mat(k, "Z", Z, false);
		fprintf(stdout, "\n");
	}

	if (H == 0) {
		return -1;
	}

	if (dt > 0) {
		SV_CREATE_STACK_MAT(F, state_cnt, state_cnt);
		sv_set_constant(&F, NAN);

		k->F_fn(dt, &F, x1);
		assert(sv_is_finite(&F));

		// Run predict
		survive_kalman_predict_covariance(dt, &F, &x2, k);
		SV_FREE_STACK_MAT(F);
	}

	// Run update; filling in K
	SV_CREATE_STACK_MAT(K, state_cnt, Z->rows);
	FLT *Rs = adaptive ? Rv : alloca(Z->rows * Z->rows * sizeof(FLT));
	SvMat R = svMat(Z->rows, Z->rows, Rs);
	if (!adaptive) {
		sv_set_diag(&R, Rv);
		if (k->datalog) {
			k->datalog(k, "R", Rv, Z->rows);
		}
	}

	survive_kalman_update_covariance(k, &K, H, &R);

	linear_update(dt, k, &y, &K, &x2, x1);

	if (k->log_level > KALMAN_LOG_LEVEL) {
		fprintf(stdout, "INFO kalman_update to    ");
		sv_print_mat(k, "x1", x1, false);
	}

	if (adaptive) {
		// https://arxiv.org/pdf/1702.00884.pdf
		SV_CREATE_STACK_MAT(PostHStorage, Z->rows, state_cnt);
		SV_CREATE_STACK_MAT(HPkHt, Z->rows, Z->rows);
		SV_CREATE_STACK_MAT(yyt, Z->rows, Z->rows);

		SvMat *PostH = survive_kalman_find_residual(dt, k, Hfn, user, Z, x1, &y, &PostHStorage);
		svMulTransposed(&y, &yyt, false, 0, 1);

		SV_CREATE_STACK_MAT(Pk_k1Ht, state_cnt, H->rows);

		svGEMM(&Pm, PostH, 1, 0, 0, &Pk_k1Ht, SV_GEMM_FLAG_B_T);
		svGEMM(PostH, &Pk_k1Ht, 1, 0, 0, &HPkHt, 0);

		sv_print_mat_v(k, 200, "PostH", PostH, true);
		sv_print_mat_v(k, 200, "PkHt", &Pk_k1Ht, true);
		sv_print_mat_v(k, 200, "HpkHt", &HPkHt, true);
		sv_print_mat_v(k, 200, "yyt", &yyt, true);

		FLT a = .3;
		FLT b = 1 - a;
		for (int i = 0; i < Z->rows; i++) {
			for (int j = 0; j < Z->rows; j++) {
				size_t idx = i + j * Z->rows;

				// HPkHt should in theory have positive diagonal but
				// rounding errors can push it over. Absolute value of it here
				// to preserve a positive diaganol in R.
				FLT HpkH = i == j ? fabs(_HPkHt[idx]) : _HPkHt[idx];
				Rs[idx] = a * Rs[idx] + b * (_yyt[idx] + HpkH);
			}
		}

		sv_print_mat_v(k, 200, "Adaptive R", &R, true);

		SV_FREE_STACK_MAT(Pk_k1Ht);
		SV_FREE_STACK_MAT(yyt);
		SV_FREE_STACK_MAT(HPkHt);
		SV_FREE_STACK_MAT(PostHStorage);
	}

	k->t = t;
	result = normnd2(_y, y.rows * y.cols);

	SV_FREE_STACK_MAT(K);
	SV_FREE_STACK_MAT(HStorage);
	SV_FREE_STACK_MAT(x2);
	SV_FREE_STACK_MAT(y);
	SV_FREE_STACK_MAT(Pm);

	return result;
}

FLT survive_kalman_predict_update_state_extended(FLT t, survive_kalman_state_t *k, const struct SvMat *Z, const FLT *R,
												 kalman_measurement_model_fn_t Hfn, void *user, bool adaptive) {
	return survive_kalman_predict_update_state_extended_adaptive_internal(t, k, Z, (FLT *)R, Hfn, user, adaptive);
}

FLT survive_kalman_predict_update_state(FLT t, survive_kalman_state_t *k, const struct SvMat *Z, const struct SvMat *H,
										const FLT *R, bool adaptive) {
	return survive_kalman_predict_update_state_extended(t, k, Z, R, 0, (void *)H, adaptive);
}

void survive_kalman_predict_state(FLT t, const survive_kalman_state_t *k, size_t start_index, size_t end_index,
								  FLT *_out) {
	SV_CREATE_STACK_MAT(tmpOut, k->state_cnt, 1);
	const SvMat *x = &k->state;

	FLT dt = t == 0. ? 0 : t - k->t;
	const FLT *copyFrom = sv_as_const_vector(&k->state);
	if (dt > 0) {
		k->Predict_fn(dt, k, x, &tmpOut);
		copyFrom = _tmpOut;
	}
	assert(_out != copyFrom);
	memcpy(_out, copyFrom + start_index, (end_index - start_index) * sizeof(FLT));
	SV_FREE_STACK_MAT(tmpOut);
}
void survive_kalman_set_P(survive_kalman_state_t *k, const FLT *p) { sv_set_diag(&k->P, p); }
