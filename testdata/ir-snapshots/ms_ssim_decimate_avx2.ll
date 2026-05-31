define dso_local range(i32 -1, 1) i32 @ms_ssim_decimate_avx2(ptr noundef readonly captures(none) %0, i32 noundef %1, i32 noundef %2, ptr noundef writeonly captures(none) %3, ptr noundef writeonly captures(address_is_null) %4, ptr noundef writeonly captures(address_is_null) %5) local_unnamed_addr {
  %7 = sdiv i32 %1, 2
  %8 = and i32 %1, 1
  %9 = add nsw i32 %7, %8
  %10 = sdiv i32 %2, 2
  %11 = and i32 %2, 1
  %12 = add nsw i32 %10, %11
  %13 = sext i32 %9 to i64
  %14 = sext i32 %2 to i64
  %15 = shl nsw i64 %14, 2
  %16 = mul i64 %15, %13
  %17 = tail call noalias ptr @malloc(i64 noundef %16)
  %18 = icmp eq ptr %17, null
  br i1 %18, label %242, label %19

19:                                               ; preds = %6
  %20 = icmp sgt i32 %1, 37
  br i1 %20, label %21, label %27

21:                                               ; preds = %19
  %22 = add nsw i32 %1, -20
  %23 = lshr i32 %22, 1
  %24 = add nsw i32 %23, -9
  %25 = and i32 %24, -8
  %26 = add nuw nsw i32 %25, 10
  br label %27

27:                                               ; preds = %19, %21
  %28 = phi i32 [ %26, %21 ], [ 2, %19 ]
  %29 = tail call i32 @llvm.smin.i32(i32 %28, i32 range(i32 -1073741824, 1073741825) %9)
  %30 = tail call i32 @llvm.smin.i32(i32 range(i32 -1073741824, 1073741825) %9, i32 2)
  %31 = icmp sgt i32 %2, 0
  br i1 %31, label %32, label %39

32:                                               ; preds = %27
  %33 = sext i32 %1 to i64
  %34 = icmp sgt i32 %9, 0
  %35 = zext nneg i32 %30 to i64
  %36 = zext nneg i32 %29 to i64
  %37 = zext nneg i32 %9 to i64
  %38 = zext nneg i32 %2 to i64
  br label %49

39:                                               ; preds = %143, %27
  %40 = icmp sgt i32 %12, 0
  br i1 %40, label %41, label %146

41:                                               ; preds = %39
  %42 = add nsw i32 %2, -5
  %43 = sdiv i32 %42, 2
  %44 = icmp sgt i32 %9, 0
  %45 = zext nneg i32 %9 to i64
  %46 = icmp slt i32 %9, 8
  %47 = sext i32 %43 to i64
  %48 = zext nneg i32 %12 to i64
  br label %148

49:                                               ; preds = %32, %143
  %50 = phi i64 [ 0, %32 ], [ %144, %143 ]
  %51 = mul nsw i64 %50, %33
  %52 = getelementptr inbounds nuw float, ptr %0, i64 %51
  %53 = mul nsw i64 %50, %13
  %54 = getelementptr inbounds nuw float, ptr %17, i64 %53
  br i1 %34, label %62, label %55

55:                                               ; preds = %62, %49
  %56 = phi i32 [ 0, %49 ], [ %30, %62 ]
  %57 = add nuw nsw i32 %56, 8
  %58 = icmp sgt i32 %57, %29
  br i1 %58, label %71, label %59

59:                                               ; preds = %55
  %60 = zext nneg i32 %56 to i64
  %61 = add nuw nsw i64 %60, 8
  br label %76

62:                                               ; preds = %49, %62
  %63 = phi i64 [ %67, %62 ], [ 0, %49 ]
  %64 = trunc nuw nsw i64 %63 to i32
  %65 = tail call fastcc float @h_pass_scalar(ptr noundef readonly %52, i32 noundef %64, i32 noundef %1)
  %66 = getelementptr inbounds nuw float, ptr %54, i64 %63
  store float %65, ptr %66, align 4
  %67 = add nuw nsw i64 %63, 1
  %68 = icmp eq i64 %67, %35
  br i1 %68, label %55, label %62, !llvm.loop !11

69:                                               ; preds = %76
  %70 = trunc nuw nsw i64 %78 to i32
  br label %71

71:                                               ; preds = %69, %55
  %72 = phi i32 [ %56, %55 ], [ %70, %69 ]
  %73 = icmp slt i32 %72, %9
  br i1 %73, label %74, label %143

74:                                               ; preds = %71
  %75 = zext nneg i32 %72 to i64
  br label %136

76:                                               ; preds = %76, %59
  %77 = phi i64 [ %60, %59 ], [ %135, %76 ]
  %78 = phi i64 [ %61, %59 ], [ %133, %76 ]
  %79 = shl i64 %77, 3
  %80 = getelementptr i8, ptr %52, i64 %79
  %81 = getelementptr i8, ptr %80, i64 -16
  %82 = load <8 x float>, ptr %81, align 1
  %83 = getelementptr i8, ptr %80, i64 16
  %84 = load <8 x float>, ptr %83, align 1
  %85 = shufflevector <8 x float> %82, <8 x float> %84, <8 x i32> <i32 0, i32 2, i32 4, i32 6, i32 8, i32 10, i32 12, i32 14>
  %86 = tail call <8 x float> @llvm.fma.v8f32(<8 x float> %85, <8 x float> splat (float 0x3F9B5E52A0000000), <8 x float> zeroinitializer)
  %87 = getelementptr i8, ptr %80, i64 -12
  %88 = load <8 x float>, ptr %87, align 1
  %89 = getelementptr i8, ptr %80, i64 20
  %90 = load <8 x float>, ptr %89, align 1
  %91 = shufflevector <8 x float> %88, <8 x float> %90, <8 x i32> <i32 0, i32 2, i32 4, i32 6, i32 8, i32 10, i32 12, i32 14>
  %92 = tail call <8 x float> @llvm.fma.v8f32(<8 x float> %91, <8 x float> splat (float 0xBF913B5C00000000), <8 x float> %86)
  %93 = getelementptr i8, ptr %80, i64 -8
  %94 = load <8 x float>, ptr %93, align 1
  %95 = getelementptr i8, ptr %80, i64 24
  %96 = load <8 x float>, ptr %95, align 1
  %97 = shufflevector <8 x float> %94, <8 x float> %96, <8 x i32> <i32 0, i32 2, i32 4, i32 6, i32 8, i32 10, i32 12, i32 14>
  %98 = tail call <8 x float> @llvm.fma.v8f32(<8 x float> %97, <8 x float> splat (float 0xBFB404FB20000000), <8 x float> %92)
  %99 = getelementptr i8, ptr %80, i64 -4
  %100 = load <8 x float>, ptr %99, align 1
  %101 = getelementptr i8, ptr %80, i64 28
  %102 = load <8 x float>, ptr %101, align 1
  %103 = shufflevector <8 x float> %100, <8 x float> %102, <8 x i32> <i32 0, i32 2, i32 4, i32 6, i32 8, i32 10, i32 12, i32 14>
  %104 = tail call <8 x float> @llvm.fma.v8f32(<8 x float> %103, <8 x float> splat (float 0x3FD1140140000000), <8 x float> %98)
  %105 = load <8 x float>, ptr %80, align 1
  %106 = getelementptr i8, ptr %80, i64 32
  %107 = load <8 x float>, ptr %106, align 1
  %108 = shufflevector <8 x float> %105, <8 x float> %107, <8 x i32> <i32 0, i32 2, i32 4, i32 6, i32 8, i32 10, i32 12, i32 14>
  %109 = tail call <8 x float> @llvm.fma.v8f32(<8 x float> %108, <8 x float> splat (float 0x3FE34B1240000000), <8 x float> %104)
  %110 = getelementptr i8, ptr %80, i64 4
  %111 = load <8 x float>, ptr %110, align 1
  %112 = getelementptr i8, ptr %80, i64 36
  %113 = load <8 x float>, ptr %112, align 1
  %114 = shufflevector <8 x float> %111, <8 x float> %113, <8 x i32> <i32 0, i32 2, i32 4, i32 6, i32 8, i32 10, i32 12, i32 14>
  %115 = tail call <8 x float> @llvm.fma.v8f32(<8 x float> %114, <8 x float> splat (float 0x3FD1140140000000), <8 x float> %109)
  %116 = getelementptr i8, ptr %80, i64 8
  %117 = load <8 x float>, ptr %116, align 1
  %118 = getelementptr i8, ptr %80, i64 40
  %119 = load <8 x float>, ptr %118, align 1
  %120 = shufflevector <8 x float> %117, <8 x float> %119, <8 x i32> <i32 0, i32 2, i32 4, i32 6, i32 8, i32 10, i32 12, i32 14>
  %121 = tail call <8 x float> @llvm.fma.v8f32(<8 x float> %120, <8 x float> splat (float 0xBFB404FB20000000), <8 x float> %115)
  %122 = getelementptr i8, ptr %80, i64 12
  %123 = load <8 x float>, ptr %122, align 1
  %124 = getelementptr i8, ptr %80, i64 44
  %125 = load <8 x float>, ptr %124, align 1
  %126 = shufflevector <8 x float> %123, <8 x float> %125, <8 x i32> <i32 0, i32 2, i32 4, i32 6, i32 8, i32 10, i32 12, i32 14>
  %127 = tail call <8 x float> @llvm.fma.v8f32(<8 x float> %126, <8 x float> splat (float 0xBF913B5C00000000), <8 x float> %121)
  %128 = getelementptr i8, ptr %80, i64 48
  %129 = load <8 x float>, ptr %128, align 1
  %130 = shufflevector <8 x float> %84, <8 x float> %129, <8 x i32> <i32 0, i32 2, i32 4, i32 6, i32 8, i32 10, i32 12, i32 14>
  %131 = tail call <8 x float> @llvm.fma.v8f32(<8 x float> %130, <8 x float> splat (float 0x3F9B5E52A0000000), <8 x float> %127)
  %132 = getelementptr inbounds nuw float, ptr %54, i64 %77
  store <8 x float> %131, ptr %132, align 1
  %133 = add nuw nsw i64 %78, 8
  %134 = icmp samesign ugt i64 %133, %36
  %135 = add nuw nsw i64 %77, 8
  br i1 %134, label %69, label %76, !llvm.loop !14

136:                                              ; preds = %136, %74
  %137 = phi i64 [ %75, %74 ], [ %141, %136 ]
  %138 = trunc nuw nsw i64 %137 to i32
  %139 = tail call fastcc float @h_pass_scalar(ptr noundef readonly %52, i32 noundef %138, i32 noundef %1)
  %140 = getelementptr inbounds nuw float, ptr %54, i64 %137
  store float %139, ptr %140, align 4
  %141 = add nuw nsw i64 %137, 1
  %142 = icmp eq i64 %141, %37
  br i1 %142, label %143, label %136, !llvm.loop !15

143:                                              ; preds = %136, %71
  %144 = add nuw nsw i64 %50, 1
  %145 = icmp eq i64 %144, %38
  br i1 %145, label %39, label %49, !llvm.loop !16

146:                                              ; preds = %235, %39
  tail call void @free(ptr noundef %17)
  %147 = icmp eq ptr %4, null
  br i1 %147, label %239, label %238

148:                                              ; preds = %41, %235
  %149 = phi i64 [ 0, %41 ], [ %236, %235 ]
  %150 = mul nsw i64 %149, %13
  %151 = getelementptr inbounds nuw float, ptr %3, i64 %150
  %152 = icmp samesign ugt i64 %149, 1
  %153 = icmp sle i64 %149, %47
  %154 = and i1 %152, %153
  br i1 %154, label %158, label %155

155:                                              ; preds = %148
  br i1 %44, label %156, label %235

156:                                              ; preds = %155
  %157 = trunc nuw nsw i64 %149 to i32
  br label %178

158:                                              ; preds = %148
  br i1 %46, label %187, label %159

159:                                              ; preds = %158
  %160 = shl nuw nsw i64 %149, 1
  %161 = add nsw i64 %160, -4
  %162 = mul nuw nsw i64 %161, %45
  %163 = add nsw i64 %160, -3
  %164 = mul nuw nsw i64 %163, %45
  %165 = add nsw i64 %160, -2
  %166 = mul nuw nsw i64 %165, %45
  %167 = add nsw i64 %160, -1
  %168 = mul nuw nsw i64 %167, %45
  %169 = mul nuw nsw i64 %160, %45
  %170 = or disjoint i64 %160, 1
  %171 = mul nuw nsw i64 %170, %45
  %172 = add nuw nsw i64 %160, 2
  %173 = mul nuw nsw i64 %172, %45
  %174 = add nuw nsw i64 %160, 3
  %175 = mul nuw nsw i64 %174, %45
  %176 = add nuw nsw i64 %160, 4
  %177 = mul nuw nsw i64 %176, %45
  br label %193

178:                                              ; preds = %156, %178
  %179 = phi i64 [ %183, %178 ], [ 0, %156 ]
  %180 = trunc nuw nsw i64 %179 to i32
  %181 = tail call fastcc float @v_pass_scalar(ptr noundef nonnull readonly %17, i32 noundef range(i32 -2147483648, 1073741824) %157, i32 noundef %180, i32 noundef range(i32 -1073741824, 1073741825) %9, i32 noundef %2)
  %182 = getelementptr inbounds nuw float, ptr %151, i64 %179
  store float %181, ptr %182, align 4
  %183 = add nuw nsw i64 %179, 1
  %184 = icmp eq i64 %183, %45
  br i1 %184, label %235, label %178, !llvm.loop !17

185:                                              ; preds = %193
  %186 = trunc nuw nsw i64 %195 to i32
  br label %187

187:                                              ; preds = %185, %158
  %188 = phi i32 [ 0, %158 ], [ %186, %185 ]
  %189 = icmp slt i32 %188, %9
  br i1 %189, label %190, label %235

190:                                              ; preds = %187
  %191 = zext nneg i32 %188 to i64
  %192 = trunc nuw nsw i64 %149 to i32
  br label %228

193:                                              ; preds = %193, %159
  %194 = phi i64 [ 0, %159 ], [ %227, %193 ]
  %195 = phi i64 [ 8, %159 ], [ %225, %193 ]
  %196 = getelementptr float, ptr %17, i64 %194
  %197 = getelementptr float, ptr %196, i64 %162
  %198 = load <8 x float>, ptr %197, align 1
  %199 = tail call <8 x float> @llvm.fma.v8f32(<8 x float> %198, <8 x float> splat (float 0x3F9B5E52A0000000), <8 x float> zeroinitializer)
  %200 = getelementptr float, ptr %196, i64 %164
  %201 = load <8 x float>, ptr %200, align 1
  %202 = tail call <8 x float> @llvm.fma.v8f32(<8 x float> %201, <8 x float> splat (float 0xBF913B5C00000000), <8 x float> %199)
  %203 = getelementptr float, ptr %196, i64 %166
  %204 = load <8 x float>, ptr %203, align 1
  %205 = tail call <8 x float> @llvm.fma.v8f32(<8 x float> %204, <8 x float> splat (float 0xBFB404FB20000000), <8 x float> %202)
  %206 = getelementptr float, ptr %196, i64 %168
  %207 = load <8 x float>, ptr %206, align 1
  %208 = tail call <8 x float> @llvm.fma.v8f32(<8 x float> %207, <8 x float> splat (float 0x3FD1140140000000), <8 x float> %205)
  %209 = getelementptr float, ptr %196, i64 %169
  %210 = load <8 x float>, ptr %209, align 1
  %211 = tail call <8 x float> @llvm.fma.v8f32(<8 x float> %210, <8 x float> splat (float 0x3FE34B1240000000), <8 x float> %208)
  %212 = getelementptr float, ptr %196, i64 %171
  %213 = load <8 x float>, ptr %212, align 1
  %214 = tail call <8 x float> @llvm.fma.v8f32(<8 x float> %213, <8 x float> splat (float 0x3FD1140140000000), <8 x float> %211)
  %215 = getelementptr float, ptr %196, i64 %173
  %216 = load <8 x float>, ptr %215, align 1
  %217 = tail call <8 x float> @llvm.fma.v8f32(<8 x float> %216, <8 x float> splat (float 0xBFB404FB20000000), <8 x float> %214)
  %218 = getelementptr float, ptr %196, i64 %175
  %219 = load <8 x float>, ptr %218, align 1
  %220 = tail call <8 x float> @llvm.fma.v8f32(<8 x float> %219, <8 x float> splat (float 0xBF913B5C00000000), <8 x float> %217)
  %221 = getelementptr float, ptr %196, i64 %177
  %222 = load <8 x float>, ptr %221, align 1
  %223 = tail call <8 x float> @llvm.fma.v8f32(<8 x float> %222, <8 x float> splat (float 0x3F9B5E52A0000000), <8 x float> %220)
  %224 = getelementptr inbounds nuw float, ptr %151, i64 %194
  store <8 x float> %223, ptr %224, align 1
  %225 = add nuw nsw i64 %195, 8
  %226 = icmp samesign ugt i64 %225, %45
  %227 = add nuw nsw i64 %194, 8
  br i1 %226, label %185, label %193, !llvm.loop !18

228:                                              ; preds = %228, %190
  %229 = phi i64 [ %191, %190 ], [ %233, %228 ]
  %230 = trunc nuw nsw i64 %229 to i32
  %231 = tail call fastcc float @v_pass_scalar(ptr noundef nonnull readonly %17, i32 noundef range(i32 -2147483648, 1073741824) %192, i32 noundef %230, i32 noundef range(i32 -1073741824, 1073741825) %9, i32 noundef %2)
  %232 = getelementptr inbounds nuw float, ptr %151, i64 %229
  store float %231, ptr %232, align 4
  %233 = add nuw nsw i64 %229, 1
  %234 = icmp eq i64 %233, %45
  br i1 %234, label %235, label %228, !llvm.loop !19

235:                                              ; preds = %178, %228, %155, %187
  %236 = add nuw nsw i64 %149, 1
  %237 = icmp eq i64 %236, %48
  br i1 %237, label %146, label %148, !llvm.loop !20

238:                                              ; preds = %146
  store i32 %9, ptr %4, align 4
  br label %239

239:                                              ; preds = %238, %146
  %240 = icmp eq ptr %5, null
  br i1 %240, label %242, label %241

241:                                              ; preds = %239
  store i32 %12, ptr %5, align 4
  br label %242

242:                                              ; preds = %239, %241, %6
  %243 = phi i32 [ -1, %6 ], [ 0, %241 ], [ 0, %239 ]
  ret i32 %243
}
