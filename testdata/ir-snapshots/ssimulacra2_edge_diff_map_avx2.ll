define dso_local void @ssimulacra2_edge_diff_map_avx2(ptr noundef readonly captures(none) %0, ptr noundef readonly captures(none) %1, ptr noundef readonly captures(none) %2, ptr noundef readonly captures(none) %3, i32 noundef %4, i32 noundef %5, ptr noundef writeonly captures(none) %6) local_unnamed_addr {
  %8 = alloca [8 x float], align 32
  %9 = alloca [8 x float], align 32
  %10 = zext i32 %4 to i64
  %11 = zext i32 %5 to i64
  %12 = mul nuw i64 %11, %10
  %13 = uitofp i64 %12 to double
  %14 = fdiv double 1.000000e+00, %13
  %15 = icmp ult i64 %12, 8
  br label %17

16:                                               ; preds = %126
  ret void

17:                                               ; preds = %7, %126
  %18 = phi i64 [ 0, %7 ], [ %144, %126 ]
  %19 = mul i64 %12, %18
  %20 = getelementptr inbounds nuw float, ptr %0, i64 %19
  %21 = getelementptr inbounds nuw float, ptr %1, i64 %19
  %22 = getelementptr inbounds nuw float, ptr %2, i64 %19
  %23 = getelementptr inbounds nuw float, ptr %3, i64 %19
  br i1 %15, label %24, label %31

24:                                               ; preds = %50, %17
  %25 = phi i64 [ 0, %17 ], [ %32, %50 ]
  %26 = phi double [ 0.000000e+00, %17 ], [ %81, %50 ]
  %27 = phi double [ 0.000000e+00, %17 ], [ %78, %50 ]
  %28 = phi double [ 0.000000e+00, %17 ], [ %77, %50 ]
  %29 = phi double [ 0.000000e+00, %17 ], [ %74, %50 ]
  %30 = icmp ult i64 %25, %12
  br i1 %30, label %84, label %126

31:                                               ; preds = %17, %50
  %32 = phi i64 [ %51, %50 ], [ 8, %17 ]
  %33 = phi double [ %74, %50 ], [ 0.000000e+00, %17 ]
  %34 = phi double [ %77, %50 ], [ 0.000000e+00, %17 ]
  %35 = phi double [ %78, %50 ], [ 0.000000e+00, %17 ]
  %36 = phi double [ %81, %50 ], [ 0.000000e+00, %17 ]
  %37 = phi i64 [ %32, %50 ], [ 0, %17 ]
  %38 = getelementptr inbounds nuw float, ptr %20, i64 %37
  %39 = load <8 x float>, ptr %38, align 1
  %40 = getelementptr inbounds nuw float, ptr %22, i64 %37
  %41 = load <8 x float>, ptr %40, align 1
  %42 = getelementptr inbounds nuw float, ptr %21, i64 %37
  %43 = load <8 x float>, ptr %42, align 1
  %44 = getelementptr inbounds nuw float, ptr %23, i64 %37
  %45 = load <8 x float>, ptr %44, align 1
  %46 = fsub <8 x float> %39, %43
  %47 = tail call <8 x float> @llvm.fabs.v8f32(<8 x float> %46)
  %48 = fsub <8 x float> %41, %45
  %49 = tail call <8 x float> @llvm.fabs.v8f32(<8 x float> %48)
  call void @llvm.lifetime.start.p0(ptr nonnull %8)
  call void @llvm.lifetime.start.p0(ptr nonnull %9)
  store <8 x float> %47, ptr %8, align 32
  store <8 x float> %49, ptr %9, align 32
  br label %53

50:                                               ; preds = %53
  call void @llvm.lifetime.end.p0(ptr nonnull %9)
  call void @llvm.lifetime.end.p0(ptr nonnull %8)
  %51 = add nuw i64 %32, 8
  %52 = icmp ugt i64 %51, %12
  br i1 %52, label %24, label %31, !llvm.loop !34

53:                                               ; preds = %31, %53
  %54 = phi i64 [ 0, %31 ], [ %82, %53 ]
  %55 = phi double [ %33, %31 ], [ %74, %53 ]
  %56 = phi double [ %34, %31 ], [ %77, %53 ]
  %57 = phi double [ %35, %31 ], [ %78, %53 ]
  %58 = phi double [ %36, %31 ], [ %81, %53 ]
  %59 = getelementptr inbounds nuw float, ptr %8, i64 %54
  %60 = load float, ptr %59, align 4
  %61 = fpext float %60 to double
  %62 = getelementptr inbounds nuw float, ptr %9, i64 %54
  %63 = load float, ptr %62, align 4
  %64 = fpext float %63 to double
  %65 = fadd double %64, 1.000000e+00
  %66 = fadd double %61, 1.000000e+00
  %67 = fdiv double %65, %66
  %68 = fadd double %67, -1.000000e+00
  %69 = fcmp ogt double %68, 0.000000e+00
  %70 = select i1 %69, double %68, double 0.000000e+00
  %71 = fcmp olt double %68, 0.000000e+00
  %72 = fneg double %68
  %73 = select i1 %71, double %72, double 0.000000e+00
  %74 = fadd double %55, %70
  %75 = fmul double %70, %70
  %76 = fmul double %75, %75
  %77 = fadd double %56, %76
  %78 = fadd double %57, %73
  %79 = fmul double %73, %73
  %80 = fmul double %79, %79
  %81 = fadd double %58, %80
  %82 = add nuw nsw i64 %54, 1
  %83 = icmp eq i64 %82, 8
  br i1 %83, label %50, label %53, !llvm.loop !35

84:                                               ; preds = %24, %84
  %85 = phi double [ %116, %84 ], [ %29, %24 ]
  %86 = phi double [ %119, %84 ], [ %28, %24 ]
  %87 = phi double [ %120, %84 ], [ %27, %24 ]
  %88 = phi double [ %123, %84 ], [ %26, %24 ]
  %89 = phi i64 [ %124, %84 ], [ %25, %24 ]
  %90 = getelementptr inbounds nuw float, ptr %20, i64 %89
  %91 = load float, ptr %90, align 4
  %92 = getelementptr inbounds nuw float, ptr %21, i64 %89
  %93 = load float, ptr %92, align 4
  %94 = getelementptr inbounds nuw float, ptr %22, i64 %89
  %95 = load float, ptr %94, align 4
  %96 = getelementptr inbounds nuw float, ptr %23, i64 %89
  %97 = load float, ptr %96, align 4
  %98 = insertelement <2 x float> poison, float %95, i64 0
  %99 = insertelement <2 x float> %98, float %91, i64 1
  %100 = fpext <2 x float> %99 to <2 x double>
  %101 = insertelement <2 x float> poison, float %97, i64 0
  %102 = insertelement <2 x float> %101, float %93, i64 1
  %103 = fpext <2 x float> %102 to <2 x double>
  %104 = fsub <2 x double> %100, %103
  %105 = tail call <2 x double> @llvm.fabs.v2f64(<2 x double> %104)
  %106 = fadd <2 x double> %105, splat (double 1.000000e+00)
  %107 = shufflevector <2 x double> %106, <2 x double> poison, <2 x i32> <i32 1, i32 poison>
  %108 = fdiv <2 x double> %106, %107
  %109 = extractelement <2 x double> %108, i64 0
  %110 = fadd double %109, -1.000000e+00
  %111 = fcmp ogt double %110, 0.000000e+00
  %112 = select i1 %111, double %110, double 0.000000e+00
  %113 = fcmp olt double %110, 0.000000e+00
  %114 = fneg double %110
  %115 = select i1 %113, double %114, double 0.000000e+00
  %116 = fadd double %85, %112
  %117 = fmul double %112, %112
  %118 = fmul double %117, %117
  %119 = fadd double %86, %118
  %120 = fadd double %87, %115
  %121 = fmul double %115, %115
  %122 = fmul double %121, %121
  %123 = fadd double %88, %122
  %124 = add nuw i64 %89, 1
  %125 = icmp eq i64 %124, %12
  br i1 %125, label %126, label %84, !llvm.loop !36

126:                                              ; preds = %84, %24
  %127 = phi double [ %26, %24 ], [ %123, %84 ]
  %128 = phi double [ %27, %24 ], [ %120, %84 ]
  %129 = phi double [ %28, %24 ], [ %119, %84 ]
  %130 = phi double [ %29, %24 ], [ %116, %84 ]
  %131 = fmul double %14, %130
  %132 = shl nuw nsw i64 %18, 5
  %133 = getelementptr inbounds nuw i8, ptr %6, i64 %132
  store double %131, ptr %133, align 8
  %134 = fmul double %14, %129
  %135 = tail call double @sqrt(double noundef %134)
  %136 = tail call double @sqrt(double noundef %135)
  %137 = getelementptr inbounds nuw i8, ptr %133, i64 8
  store double %136, ptr %137, align 8
  %138 = fmul double %14, %128
  %139 = getelementptr inbounds nuw i8, ptr %133, i64 16
  store double %138, ptr %139, align 8
  %140 = fmul double %14, %127
  %141 = tail call double @sqrt(double noundef %140)
  %142 = tail call double @sqrt(double noundef %141)
  %143 = getelementptr inbounds nuw i8, ptr %133, i64 24
  store double %142, ptr %143, align 8
  %144 = add nuw nsw i64 %18, 1
  %145 = icmp eq i64 %144, 3
  br i1 %145, label %16, label %17, !llvm.loop !37
}
