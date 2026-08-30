define dso_local void @ssimulacra2_ssim_map_avx2(ptr noundef readonly captures(none) %0, ptr noundef readonly captures(none) %1, ptr noundef readonly captures(none) %2, ptr noundef readonly captures(none) %3, ptr noundef readonly captures(none) %4, i32 noundef %5, i32 noundef %6, ptr noundef writeonly captures(none) %7) local_unnamed_addr {
  %9 = zext i32 %5 to i64
  %10 = zext i32 %6 to i64
  %11 = mul nuw i64 %10, %9
  %12 = uitofp i64 %11 to double
  %13 = fdiv double 1.000000e+00, %12
  %14 = icmp ult i64 %11, 8
  br label %16

15:                                               ; preds = %170
  ret void

16:                                               ; preds = %8, %170
  %17 = phi i64 [ 0, %8 ], [ %180, %170 ]
  %18 = mul i64 %11, %17
  %19 = getelementptr inbounds nuw float, ptr %0, i64 %18
  %20 = getelementptr inbounds nuw float, ptr %1, i64 %18
  %21 = getelementptr inbounds nuw float, ptr %2, i64 %18
  %22 = getelementptr inbounds nuw float, ptr %3, i64 %18
  %23 = getelementptr inbounds nuw float, ptr %4, i64 %18
  br i1 %14, label %27, label %32

24:                                               ; preds = %32
  %25 = extractelement <2 x double> %126, i64 1
  %26 = extractelement <2 x double> %126, i64 0
  br label %27

27:                                               ; preds = %24, %16
  %28 = phi i64 [ 0, %16 ], [ %33, %24 ]
  %29 = phi double [ 0.000000e+00, %16 ], [ %25, %24 ]
  %30 = phi double [ 0.000000e+00, %16 ], [ %26, %24 ]
  %31 = icmp ult i64 %28, %11
  br i1 %31, label %129, label %170

32:                                               ; preds = %16, %32
  %33 = phi i64 [ %127, %32 ], [ 8, %16 ]
  %34 = phi i64 [ %33, %32 ], [ 0, %16 ]
  %35 = phi <2 x double> [ %126, %32 ], [ zeroinitializer, %16 ]
  %36 = getelementptr inbounds nuw float, ptr %19, i64 %34
  %37 = load <8 x float>, ptr %36, align 1
  %38 = getelementptr inbounds nuw float, ptr %20, i64 %34
  %39 = load <8 x float>, ptr %38, align 1
  %40 = fmul <8 x float> %37, %37
  %41 = fmul <8 x float> %39, %39
  %42 = fmul <8 x float> %37, %39
  %43 = fsub <8 x float> %37, %39
  %44 = fmul <8 x float> %43, %43
  %45 = fsub <8 x float> splat (float 1.000000e+00), %44
  %46 = getelementptr inbounds nuw float, ptr %23, i64 %34
  %47 = load <8 x float>, ptr %46, align 1
  %48 = fsub <8 x float> %47, %42
  %49 = fmul <8 x float> %48, splat (float 2.000000e+00)
  %50 = fadd <8 x float> %49, splat (float 0x3F4D7DBF40000000)
  %51 = getelementptr inbounds nuw float, ptr %21, i64 %34
  %52 = load <8 x float>, ptr %51, align 1
  %53 = fsub <8 x float> %52, %40
  %54 = getelementptr inbounds nuw float, ptr %22, i64 %34
  %55 = load <8 x float>, ptr %54, align 1
  %56 = fsub <8 x float> %55, %41
  %57 = fadd <8 x float> %53, %56
  %58 = fadd <8 x float> %57, splat (float 0x3F4D7DBF40000000)
  %59 = shufflevector <8 x float> %45, <8 x float> poison, <2 x i32> <i32 0, i32 1>
  %60 = fpext <2 x float> %59 to <2 x double>
  %61 = shufflevector <8 x float> %50, <8 x float> poison, <2 x i32> <i32 0, i32 1>
  %62 = fpext <2 x float> %61 to <2 x double>
  %63 = fmul <2 x double> %60, %62
  %64 = shufflevector <8 x float> %58, <8 x float> poison, <2 x i32> <i32 0, i32 1>
  %65 = fpext <2 x float> %64 to <2 x double>
  %66 = fdiv <2 x double> %63, %65
  %67 = fsub <2 x double> splat (double 1.000000e+00), %66
  %68 = fcmp olt <2 x double> %67, zeroinitializer
  %69 = select <2 x i1> %68, <2 x double> zeroinitializer, <2 x double> %67
  %70 = fmul <2 x double> %69, %69
  %71 = fmul <2 x double> %70, %70
  %72 = shufflevector <8 x float> %45, <8 x float> poison, <2 x i32> <i32 2, i32 3>
  %73 = fpext <2 x float> %72 to <2 x double>
  %74 = shufflevector <8 x float> %50, <8 x float> poison, <2 x i32> <i32 2, i32 3>
  %75 = fpext <2 x float> %74 to <2 x double>
  %76 = fmul <2 x double> %73, %75
  %77 = shufflevector <8 x float> %58, <8 x float> poison, <2 x i32> <i32 2, i32 3>
  %78 = fpext <2 x float> %77 to <2 x double>
  %79 = fdiv <2 x double> %76, %78
  %80 = fsub <2 x double> splat (double 1.000000e+00), %79
  %81 = fcmp olt <2 x double> %80, zeroinitializer
  %82 = select <2 x i1> %81, <2 x double> zeroinitializer, <2 x double> %80
  %83 = fmul <2 x double> %82, %82
  %84 = fmul <2 x double> %83, %83
  %85 = shufflevector <8 x float> %45, <8 x float> poison, <2 x i32> <i32 4, i32 5>
  %86 = fpext <2 x float> %85 to <2 x double>
  %87 = shufflevector <8 x float> %50, <8 x float> poison, <2 x i32> <i32 4, i32 5>
  %88 = fpext <2 x float> %87 to <2 x double>
  %89 = fmul <2 x double> %86, %88
  %90 = shufflevector <8 x float> %58, <8 x float> poison, <2 x i32> <i32 4, i32 5>
  %91 = fpext <2 x float> %90 to <2 x double>
  %92 = fdiv <2 x double> %89, %91
  %93 = fsub <2 x double> splat (double 1.000000e+00), %92
  %94 = fcmp olt <2 x double> %93, zeroinitializer
  %95 = select <2 x i1> %94, <2 x double> zeroinitializer, <2 x double> %93
  %96 = fmul <2 x double> %95, %95
  %97 = fmul <2 x double> %96, %96
  %98 = shufflevector <8 x float> %45, <8 x float> poison, <2 x i32> <i32 6, i32 7>
  %99 = fpext <2 x float> %98 to <2 x double>
  %100 = shufflevector <8 x float> %50, <8 x float> poison, <2 x i32> <i32 6, i32 7>
  %101 = fpext <2 x float> %100 to <2 x double>
  %102 = fmul <2 x double> %99, %101
  %103 = shufflevector <8 x float> %58, <8 x float> poison, <2 x i32> <i32 6, i32 7>
  %104 = fpext <2 x float> %103 to <2 x double>
  %105 = fdiv <2 x double> %102, %104
  %106 = fsub <2 x double> splat (double 1.000000e+00), %105
  %107 = fcmp olt <2 x double> %106, zeroinitializer
  %108 = select <2 x i1> %107, <2 x double> zeroinitializer, <2 x double> %106
  %109 = fmul <2 x double> %108, %108
  %110 = fmul <2 x double> %109, %109
  %111 = shufflevector <2 x double> %69, <2 x double> %71, <2 x i32> <i32 0, i32 2>
  %112 = fadd <2 x double> %35, %111
  %113 = shufflevector <2 x double> %69, <2 x double> %71, <2 x i32> <i32 1, i32 3>
  %114 = fadd <2 x double> %112, %113
  %115 = shufflevector <2 x double> %82, <2 x double> %84, <2 x i32> <i32 0, i32 2>
  %116 = fadd <2 x double> %114, %115
  %117 = shufflevector <2 x double> %82, <2 x double> %84, <2 x i32> <i32 1, i32 3>
  %118 = fadd <2 x double> %116, %117
  %119 = shufflevector <2 x double> %95, <2 x double> %97, <2 x i32> <i32 0, i32 2>
  %120 = fadd <2 x double> %118, %119
  %121 = shufflevector <2 x double> %95, <2 x double> %97, <2 x i32> <i32 1, i32 3>
  %122 = fadd <2 x double> %120, %121
  %123 = shufflevector <2 x double> %108, <2 x double> %110, <2 x i32> <i32 0, i32 2>
  %124 = fadd <2 x double> %122, %123
  %125 = shufflevector <2 x double> %108, <2 x double> %110, <2 x i32> <i32 1, i32 3>
  %126 = fadd <2 x double> %124, %125
  %127 = add nuw i64 %33, 8
  %128 = icmp ugt i64 %127, %11
  br i1 %128, label %24, label %32, !llvm.loop !29

129:                                              ; preds = %27, %129
  %130 = phi double [ %164, %129 ], [ %30, %27 ]
  %131 = phi double [ %167, %129 ], [ %29, %27 ]
  %132 = phi i64 [ %168, %129 ], [ %28, %27 ]
  %133 = getelementptr inbounds nuw float, ptr %19, i64 %132
  %134 = load float, ptr %133, align 4
  %135 = getelementptr inbounds nuw float, ptr %20, i64 %132
  %136 = load float, ptr %135, align 4
  %137 = fmul float %134, %134
  %138 = fmul float %136, %136
  %139 = fmul float %134, %136
  %140 = fsub float %134, %136
  %141 = fmul float %140, %140
  %142 = fsub float 1.000000e+00, %141
  %143 = getelementptr inbounds nuw float, ptr %23, i64 %132
  %144 = load float, ptr %143, align 4
  %145 = fsub float %144, %139
  %146 = fmul float %145, 2.000000e+00
  %147 = fadd float %146, 0x3F4D7DBF40000000
  %148 = getelementptr inbounds nuw float, ptr %21, i64 %132
  %149 = load float, ptr %148, align 4
  %150 = fsub float %149, %137
  %151 = getelementptr inbounds nuw float, ptr %22, i64 %132
  %152 = load float, ptr %151, align 4
  %153 = fsub float %152, %138
  %154 = fadd float %150, %153
  %155 = fadd float %154, 0x3F4D7DBF40000000
  %156 = fpext float %142 to double
  %157 = fpext float %147 to double
  %158 = fmul double %156, %157
  %159 = fpext float %155 to double
  %160 = fdiv double %158, %159
  %161 = fsub double 1.000000e+00, %160
  %162 = fcmp olt double %161, 0.000000e+00
  %163 = select i1 %162, double 0.000000e+00, double %161
  %164 = fadd double %130, %163
  %165 = fmul double %163, %163
  %166 = fmul double %165, %165
  %167 = fadd double %131, %166
  %168 = add nuw i64 %132, 1
  %169 = icmp eq i64 %168, %11
  br i1 %169, label %170, label %129, !llvm.loop !30

170:                                              ; preds = %129, %27
  %171 = phi double [ %29, %27 ], [ %167, %129 ]
  %172 = phi double [ %30, %27 ], [ %164, %129 ]
  %173 = fmul double %13, %172
  %174 = shl nuw nsw i64 %17, 4
  %175 = getelementptr inbounds nuw i8, ptr %7, i64 %174
  store double %173, ptr %175, align 8
  %176 = fmul double %13, %171
  %177 = tail call double @sqrt(double noundef %176)
  %178 = tail call double @sqrt(double noundef %177)
  %179 = getelementptr inbounds nuw i8, ptr %175, i64 8
  store double %178, ptr %179, align 8
  %180 = add nuw nsw i64 %17, 1
  %181 = icmp eq i64 %180, 3
  br i1 %181, label %15, label %16, !llvm.loop !33
}
