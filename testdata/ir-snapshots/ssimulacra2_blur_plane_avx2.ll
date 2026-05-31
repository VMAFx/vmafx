define dso_local void @ssimulacra2_blur_plane_avx2(ptr noundef readonly captures(none) %0, ptr noundef readonly captures(none) %1, i32 noundef %2, ptr noundef captures(address_is_null) %3, ptr noundef %4, ptr noundef writeonly captures(address_is_null) %5, ptr noundef captures(address_is_null) %6, i32 noundef %7, i32 noundef %8) local_unnamed_addr {
  %10 = alloca [8 x float], align 32
  %11 = icmp eq ptr %3, null
  br i1 %11, label %12, label %13

12:                                               ; preds = %9
  tail call void @__assert_fail(ptr noundef nonnull @.str.4, ptr noundef nonnull @.str.1, i32 noundef 623, ptr noundef nonnull @__PRETTY_FUNCTION__.ssimulacra2_blur_plane_avx2)
  unreachable

13:                                               ; preds = %9
  %14 = icmp eq ptr %4, null
  br i1 %14, label %15, label %16

15:                                               ; preds = %13
  tail call void @__assert_fail(ptr noundef nonnull @.str.5, ptr noundef nonnull @.str.1, i32 noundef 624, ptr noundef nonnull @__PRETTY_FUNCTION__.ssimulacra2_blur_plane_avx2)
  unreachable

16:                                               ; preds = %13
  %17 = icmp eq ptr %5, null
  br i1 %17, label %18, label %19

18:                                               ; preds = %16
  tail call void @__assert_fail(ptr noundef nonnull @.str.6, ptr noundef nonnull @.str.1, i32 noundef 625, ptr noundef nonnull @__PRETTY_FUNCTION__.ssimulacra2_blur_plane_avx2)
  unreachable

19:                                               ; preds = %16
  %20 = icmp eq ptr %6, null
  br i1 %20, label %21, label %22

21:                                               ; preds = %19
  tail call void @__assert_fail(ptr noundef nonnull @.str.7, ptr noundef nonnull @.str.1, i32 noundef 626, ptr noundef nonnull @__PRETTY_FUNCTION__.ssimulacra2_blur_plane_avx2)
  unreachable

22:                                               ; preds = %19
  %23 = icmp ne i32 %7, 0
  %24 = icmp ne i32 %8, 0
  %25 = and i1 %23, %24
  br i1 %25, label %26, label %40

26:                                               ; preds = %22
  %27 = icmp ult i32 %8, 8
  br i1 %27, label %146, label %28

28:                                               ; preds = %26
  %29 = sext i32 %2 to i64
  %30 = zext i32 %7 to i64
  %31 = getelementptr inbounds nuw i8, ptr %0, i64 4
  %32 = getelementptr inbounds nuw i8, ptr %0, i64 8
  %33 = getelementptr inbounds nuw i8, ptr %1, i64 4
  %34 = getelementptr inbounds nuw i8, ptr %1, i64 8
  %35 = sub nsw i64 1, %29
  %36 = icmp slt i64 %35, %30
  %37 = xor i64 %29, -1
  %38 = insertelement <8 x i32> poison, i32 %7, i64 0
  %39 = shufflevector <8 x i32> %38, <8 x i32> poison, <8 x i32> zeroinitializer
  br label %41

40:                                               ; preds = %22
  tail call void @__assert_fail(ptr noundef nonnull @.str.3, ptr noundef nonnull @.str.1, i32 noundef 627, ptr noundef nonnull @__PRETTY_FUNCTION__.ssimulacra2_blur_plane_avx2)
  unreachable

41:                                               ; preds = %28, %143
  %42 = phi i32 [ 8, %28 ], [ %144, %143 ]
  %43 = phi i32 [ 0, %28 ], [ %42, %143 ]
  %44 = insertelement <8 x i32> poison, i32 %43, i64 0
  %45 = shufflevector <8 x i32> %44, <8 x i32> poison, <8 x i32> zeroinitializer
  %46 = or disjoint <8 x i32> %45, <i32 0, i32 1, i32 2, i32 3, i32 4, i32 5, i32 6, i32 7>
  %47 = mul <8 x i32> %46, %39
  %48 = load float, ptr %0, align 4
  %49 = insertelement <8 x float> poison, float %48, i64 0
  %50 = shufflevector <8 x float> %49, <8 x float> poison, <8 x i32> zeroinitializer
  %51 = load float, ptr %31, align 4
  %52 = insertelement <8 x float> poison, float %51, i64 0
  %53 = shufflevector <8 x float> %52, <8 x float> poison, <8 x i32> zeroinitializer
  %54 = load float, ptr %32, align 4
  %55 = insertelement <8 x float> poison, float %54, i64 0
  %56 = shufflevector <8 x float> %55, <8 x float> poison, <8 x i32> zeroinitializer
  %57 = load float, ptr %1, align 4
  %58 = insertelement <8 x float> poison, float %57, i64 0
  %59 = shufflevector <8 x float> %58, <8 x float> poison, <8 x i32> zeroinitializer
  %60 = load float, ptr %33, align 4
  %61 = insertelement <8 x float> poison, float %60, i64 0
  %62 = shufflevector <8 x float> %61, <8 x float> poison, <8 x i32> zeroinitializer
  %63 = load float, ptr %34, align 4
  %64 = insertelement <8 x float> poison, float %63, i64 0
  %65 = shufflevector <8 x float> %64, <8 x float> poison, <8 x i32> zeroinitializer
  br i1 %36, label %66, label %143

66:                                               ; preds = %41
  %67 = zext i32 %43 to i64
  %68 = mul nuw i64 %67, %30
  %69 = or disjoint i64 %67, 1
  %70 = mul nuw i64 %69, %30
  %71 = or disjoint i64 %67, 2
  %72 = mul nuw i64 %71, %30
  %73 = or disjoint i64 %67, 3
  %74 = mul nuw i64 %73, %30
  %75 = or disjoint i64 %67, 4
  %76 = mul nuw i64 %75, %30
  %77 = or disjoint i64 %67, 5
  %78 = mul nuw i64 %77, %30
  %79 = or disjoint i64 %67, 6
  %80 = mul nuw i64 %79, %30
  %81 = or disjoint i64 %67, 7
  %82 = mul nuw i64 %81, %30
  br label %83

83:                                               ; preds = %140, %66
  %84 = phi <8 x float> [ zeroinitializer, %66 ], [ %110, %140 ]
  %85 = phi <8 x float> [ zeroinitializer, %66 ], [ %114, %140 ]
  %86 = phi <8 x float> [ zeroinitializer, %66 ], [ %118, %140 ]
  %87 = phi <8 x float> [ zeroinitializer, %66 ], [ %84, %140 ]
  %88 = phi <8 x float> [ zeroinitializer, %66 ], [ %85, %140 ]
  %89 = phi i64 [ %35, %66 ], [ %141, %140 ]
  %90 = phi <8 x float> [ zeroinitializer, %66 ], [ %86, %140 ]
  %91 = add i64 %89, %37
  %92 = add nsw i64 %89, %29
  %93 = icmp sgt i64 %91, -1
  br i1 %93, label %94, label %97

94:                                               ; preds = %83
  %95 = getelementptr inbounds nuw float, ptr %4, i64 %91
  %96 = tail call <8 x float> @llvm.x86.avx2.gather.d.ps.256(<8 x float> zeroinitializer, ptr nonnull %95, <8 x i32> %47, <8 x float> splat (float 0xFFFFFFFFE0000000), i8 4)
  br label %97

97:                                               ; preds = %94, %83
  %98 = phi <8 x float> [ %96, %94 ], [ zeroinitializer, %83 ]
  %99 = icmp samesign ugt i64 %92, %30
  br i1 %99, label %104, label %100

100:                                              ; preds = %97
  %101 = getelementptr float, ptr %4, i64 %92
  %102 = getelementptr i8, ptr %101, i64 -4
  %103 = tail call <8 x float> @llvm.x86.avx2.gather.d.ps.256(<8 x float> zeroinitializer, ptr %102, <8 x i32> %47, <8 x float> splat (float 0xFFFFFFFFE0000000), i8 4)
  br label %104

104:                                              ; preds = %100, %97
  %105 = phi <8 x float> [ %103, %100 ], [ zeroinitializer, %97 ]
  %106 = fadd <8 x float> %98, %105
  %107 = fmul <8 x float> %50, %106
  %108 = fmul <8 x float> %59, %84
  %109 = fsub <8 x float> %107, %108
  %110 = fsub <8 x float> %109, %87
  %111 = fmul <8 x float> %53, %106
  %112 = fmul <8 x float> %62, %85
  %113 = fsub <8 x float> %111, %112
  %114 = fsub <8 x float> %113, %88
  %115 = fmul <8 x float> %56, %106
  %116 = fmul <8 x float> %65, %86
  %117 = fsub <8 x float> %115, %116
  %118 = fsub <8 x float> %117, %90
  %119 = icmp sgt i64 %89, -1
  br i1 %119, label %120, label %140

120:                                              ; preds = %104
  %121 = fadd <8 x float> %110, %114
  %122 = fadd <8 x float> %118, %121
  %123 = getelementptr float, ptr %6, i64 %89
  %124 = extractelement <8 x float> %122, i64 0
  %125 = getelementptr float, ptr %123, i64 %68
  store float %124, ptr %125, align 4
  %126 = extractelement <8 x float> %122, i64 1
  %127 = getelementptr float, ptr %123, i64 %70
  store float %126, ptr %127, align 4
  %128 = extractelement <8 x float> %122, i64 2
  %129 = getelementptr float, ptr %123, i64 %72
  store float %128, ptr %129, align 4
  %130 = extractelement <8 x float> %122, i64 3
  %131 = getelementptr float, ptr %123, i64 %74
  store float %130, ptr %131, align 4
  %132 = extractelement <8 x float> %122, i64 4
  %133 = getelementptr float, ptr %123, i64 %76
  store float %132, ptr %133, align 4
  %134 = extractelement <8 x float> %122, i64 5
  %135 = getelementptr float, ptr %123, i64 %78
  store float %134, ptr %135, align 4
  %136 = extractelement <8 x float> %122, i64 6
  %137 = getelementptr float, ptr %123, i64 %80
  store float %136, ptr %137, align 4
  %138 = extractelement <8 x float> %122, i64 7
  %139 = getelementptr float, ptr %123, i64 %82
  store float %138, ptr %139, align 4
  br label %140

140:                                              ; preds = %120, %104
  %141 = add nsw i64 %89, 1
  %142 = icmp eq i64 %141, %30
  br i1 %142, label %143, label %83, !llvm.loop !38

143:                                              ; preds = %140, %41
  %144 = add i32 %42, 8
  %145 = icmp ugt i32 %144, %8
  br i1 %145, label %146, label %41, !llvm.loop !39

146:                                              ; preds = %143, %26
  %147 = phi i32 [ 0, %26 ], [ %42, %143 ]
  %148 = icmp ult i32 %147, %8
  br i1 %148, label %153, label %149

149:                                              ; preds = %146
  %150 = zext i32 %7 to i64
  %151 = sext i32 %2 to i64
  %152 = sub nsw i64 1, %151
  br label %328

153:                                              ; preds = %146
  %154 = sub nuw i32 %8, %147
  %155 = insertelement <8 x i32> poison, i32 %154, i64 0
  %156 = shufflevector <8 x i32> %155, <8 x i32> poison, <8 x i32> zeroinitializer
  %157 = icmp sgt <8 x i32> %156, <i32 0, i32 1, i32 2, i32 3, i32 4, i32 5, i32 6, i32 7>
  %158 = insertelement <8 x i32> poison, i32 %147, i64 0
  %159 = shufflevector <8 x i32> %158, <8 x i32> poison, <8 x i32> zeroinitializer
  %160 = or disjoint <8 x i32> %159, <i32 0, i32 1, i32 2, i32 3, i32 4, i32 5, i32 6, i32 7>
  %161 = insertelement <8 x i32> poison, i32 %7, i64 0
  %162 = shufflevector <8 x i32> %161, <8 x i32> poison, <8 x i32> zeroinitializer
  %163 = mul <8 x i32> %160, %162
  %164 = select <8 x i1> %157, <8 x i32> %163, <8 x i32> zeroinitializer
  %165 = sext i32 %2 to i64
  %166 = zext i32 %7 to i64
  %167 = load float, ptr %0, align 4
  %168 = insertelement <8 x float> poison, float %167, i64 0
  %169 = shufflevector <8 x float> %168, <8 x float> poison, <8 x i32> zeroinitializer
  %170 = getelementptr inbounds nuw i8, ptr %0, i64 4
  %171 = load float, ptr %170, align 4
  %172 = insertelement <8 x float> poison, float %171, i64 0
  %173 = shufflevector <8 x float> %172, <8 x float> poison, <8 x i32> zeroinitializer
  %174 = getelementptr inbounds nuw i8, ptr %0, i64 8
  %175 = load float, ptr %174, align 4
  %176 = insertelement <8 x float> poison, float %175, i64 0
  %177 = shufflevector <8 x float> %176, <8 x float> poison, <8 x i32> zeroinitializer
  %178 = load float, ptr %1, align 4
  %179 = insertelement <8 x float> poison, float %178, i64 0
  %180 = shufflevector <8 x float> %179, <8 x float> poison, <8 x i32> zeroinitializer
  %181 = getelementptr inbounds nuw i8, ptr %1, i64 4
  %182 = load float, ptr %181, align 4
  %183 = insertelement <8 x float> poison, float %182, i64 0
  %184 = shufflevector <8 x float> %183, <8 x float> poison, <8 x i32> zeroinitializer
  %185 = getelementptr inbounds nuw i8, ptr %1, i64 8
  %186 = load float, ptr %185, align 4
  %187 = insertelement <8 x float> poison, float %186, i64 0
  %188 = shufflevector <8 x float> %187, <8 x float> poison, <8 x i32> zeroinitializer
  call void @llvm.lifetime.start.p0(ptr nonnull %10)
  %189 = sub nsw i64 1, %165
  %190 = icmp slt i64 %189, %166
  br i1 %190, label %191, label %327

191:                                              ; preds = %153
  %192 = xor i64 %165, -1
  %193 = zext i32 %147 to i64
  %194 = zext i32 %154 to i64
  %195 = icmp ugt i32 %154, 3
  %196 = icmp eq i32 %7, 1
  %197 = and i1 %195, %196
  %198 = icmp ult i32 %154, 32
  %199 = and i64 %194, 28
  %200 = and i64 %194, 4294967264
  %201 = icmp eq i64 %200, %194
  %202 = icmp eq i64 %199, 0
  %203 = and i64 %194, 4294967292
  %204 = icmp eq i64 %203, %194
  %205 = and i64 %194, 3
  %206 = icmp eq i64 %205, 0
  br label %207

207:                                              ; preds = %324, %191
  %208 = phi <8 x float> [ zeroinitializer, %191 ], [ %234, %324 ]
  %209 = phi <8 x float> [ zeroinitializer, %191 ], [ %238, %324 ]
  %210 = phi <8 x float> [ zeroinitializer, %191 ], [ %242, %324 ]
  %211 = phi <8 x float> [ zeroinitializer, %191 ], [ %208, %324 ]
  %212 = phi <8 x float> [ zeroinitializer, %191 ], [ %209, %324 ]
  %213 = phi i64 [ %189, %191 ], [ %325, %324 ]
  %214 = phi <8 x float> [ zeroinitializer, %191 ], [ %210, %324 ]
  %215 = add i64 %213, %192
  %216 = add nsw i64 %213, %165
  %217 = icmp sgt i64 %215, -1
  br i1 %217, label %218, label %221

218:                                              ; preds = %207
  %219 = getelementptr inbounds nuw float, ptr %4, i64 %215
  %220 = tail call <8 x float> @llvm.x86.avx2.gather.d.ps.256(<8 x float> zeroinitializer, ptr nonnull %219, <8 x i32> %164, <8 x float> splat (float 0xFFFFFFFFE0000000), i8 4)
  br label %221

221:                                              ; preds = %218, %207
  %222 = phi <8 x float> [ %220, %218 ], [ zeroinitializer, %207 ]
  %223 = icmp samesign ugt i64 %216, %166
  br i1 %223, label %228, label %224

224:                                              ; preds = %221
  %225 = getelementptr float, ptr %4, i64 %216
  %226 = getelementptr i8, ptr %225, i64 -4
  %227 = tail call <8 x float> @llvm.x86.avx2.gather.d.ps.256(<8 x float> zeroinitializer, ptr %226, <8 x i32> %164, <8 x float> splat (float 0xFFFFFFFFE0000000), i8 4)
  br label %228

228:                                              ; preds = %224, %221
  %229 = phi <8 x float> [ %227, %224 ], [ zeroinitializer, %221 ]
  %230 = fadd <8 x float> %222, %229
  %231 = fmul <8 x float> %169, %230
  %232 = fmul <8 x float> %180, %208
  %233 = fsub <8 x float> %231, %232
  %234 = fsub <8 x float> %233, %211
  %235 = fmul <8 x float> %173, %230
  %236 = fmul <8 x float> %184, %209
  %237 = fsub <8 x float> %235, %236
  %238 = fsub <8 x float> %237, %212
  %239 = fmul <8 x float> %177, %230
  %240 = fmul <8 x float> %188, %210
  %241 = fsub <8 x float> %239, %240
  %242 = fsub <8 x float> %241, %214
  %243 = icmp sgt i64 %213, -1
  br i1 %243, label %244, label %324

244:                                              ; preds = %228
  %245 = fadd <8 x float> %234, %238
  %246 = fadd <8 x float> %242, %245
  store <8 x float> %246, ptr %10, align 32
  %247 = getelementptr float, ptr %6, i64 %213
  br i1 %197, label %248, label %280

248:                                              ; preds = %244
  br i1 %198, label %269, label %249

249:                                              ; preds = %248
  %250 = getelementptr float, ptr %247, i64 %193
  br label %251

251:                                              ; preds = %251, %249
  %252 = phi i64 [ 0, %249 ], [ %265, %251 ]
  %253 = getelementptr inbounds nuw float, ptr %10, i64 %252
  %254 = getelementptr inbounds nuw i8, ptr %253, i64 32
  %255 = getelementptr inbounds nuw i8, ptr %253, i64 64
  %256 = getelementptr inbounds nuw i8, ptr %253, i64 96
  %257 = load <8 x float>, ptr %253, align 32
  %258 = load <8 x float>, ptr %254, align 32
  %259 = load <8 x float>, ptr %255, align 32
  %260 = load <8 x float>, ptr %256, align 32
  %261 = getelementptr float, ptr %250, i64 %252
  %262 = getelementptr i8, ptr %261, i64 32
  %263 = getelementptr i8, ptr %261, i64 64
  %264 = getelementptr i8, ptr %261, i64 96
  store <8 x float> %257, ptr %261, align 4
  store <8 x float> %258, ptr %262, align 4
  store <8 x float> %259, ptr %263, align 4
  store <8 x float> %260, ptr %264, align 4
  %265 = add nuw i64 %252, 32
  %266 = icmp eq i64 %265, %200
  br i1 %266, label %267, label %251, !llvm.loop !40

267:                                              ; preds = %251
  br i1 %201, label %324, label %268

268:                                              ; preds = %267
  br i1 %202, label %280, label %269

269:                                              ; preds = %248, %268
  %270 = phi i64 [ %200, %268 ], [ 0, %248 ]
  %271 = getelementptr float, ptr %247, i64 %193
  br label %272

272:                                              ; preds = %272, %269
  %273 = phi i64 [ %270, %269 ], [ %277, %272 ]
  %274 = getelementptr inbounds nuw float, ptr %10, i64 %273
  %275 = load <4 x float>, ptr %274, align 16
  %276 = getelementptr float, ptr %271, i64 %273
  store <4 x float> %275, ptr %276, align 4
  %277 = add nuw i64 %273, 4
  %278 = icmp eq i64 %277, %203
  br i1 %278, label %279, label %272, !llvm.loop !41

279:                                              ; preds = %272
  br i1 %204, label %324, label %280

280:                                              ; preds = %244, %268, %279
  %281 = phi i64 [ 0, %244 ], [ %200, %268 ], [ %203, %279 ]
  br i1 %206, label %293, label %282

282:                                              ; preds = %280, %282
  %283 = phi i64 [ %290, %282 ], [ %281, %280 ]
  %284 = phi i64 [ %291, %282 ], [ 0, %280 ]
  %285 = getelementptr inbounds nuw float, ptr %10, i64 %283
  %286 = load float, ptr %285, align 4
  %287 = add nuw nsw i64 %283, %193
  %288 = mul i64 %287, %166
  %289 = getelementptr float, ptr %247, i64 %288
  store float %286, ptr %289, align 4
  %290 = add nuw nsw i64 %283, 1
  %291 = add i64 %284, 1
  %292 = icmp eq i64 %291, %205
  br i1 %292, label %293, label %282, !llvm.loop !42

293:                                              ; preds = %282, %280
  %294 = phi i64 [ %281, %280 ], [ %290, %282 ]
  %295 = sub nsw i64 %281, %194
  %296 = icmp ugt i64 %295, -4
  br i1 %296, label %324, label %297

297:                                              ; preds = %293, %297
  %298 = phi i64 [ %322, %297 ], [ %294, %293 ]
  %299 = getelementptr inbounds nuw float, ptr %10, i64 %298
  %300 = load float, ptr %299, align 4
  %301 = add nuw nsw i64 %298, %193
  %302 = mul i64 %301, %166
  %303 = getelementptr float, ptr %247, i64 %302
  store float %300, ptr %303, align 4
  %304 = add nuw nsw i64 %298, 1
  %305 = getelementptr inbounds nuw float, ptr %10, i64 %304
  %306 = load float, ptr %305, align 4
  %307 = add nuw nsw i64 %304, %193
  %308 = mul i64 %307, %166
  %309 = getelementptr float, ptr %247, i64 %308
  store float %306, ptr %309, align 4
  %310 = add nuw nsw i64 %298, 2
  %311 = getelementptr inbounds nuw float, ptr %10, i64 %310
  %312 = load float, ptr %311, align 4
  %313 = add nuw nsw i64 %310, %193
  %314 = mul i64 %313, %166
  %315 = getelementptr float, ptr %247, i64 %314
  store float %312, ptr %315, align 4
  %316 = add nuw nsw i64 %298, 3
  %317 = getelementptr inbounds nuw float, ptr %10, i64 %316
  %318 = load float, ptr %317, align 4
  %319 = add nuw nsw i64 %316, %193
  %320 = mul i64 %319, %166
  %321 = getelementptr float, ptr %247, i64 %320
  store float %318, ptr %321, align 4
  %322 = add nuw nsw i64 %298, 4
  %323 = icmp eq i64 %322, %194
  br i1 %323, label %324, label %297, !llvm.loop !43

324:                                              ; preds = %293, %297, %267, %279, %228
  %325 = add nsw i64 %213, 1
  %326 = icmp eq i64 %325, %166
  br i1 %326, label %327, label %207, !llvm.loop !38

327:                                              ; preds = %324, %153
  call void @llvm.lifetime.end.p0(ptr nonnull %10)
  br label %328

328:                                              ; preds = %149, %327
  %329 = phi i64 [ %152, %149 ], [ %189, %327 ]
  %330 = phi i64 [ %151, %149 ], [ %165, %327 ]
  %331 = phi i64 [ %150, %149 ], [ %166, %327 ]
  %332 = getelementptr float, ptr %3, i64 %331
  %333 = shl nuw nsw i64 %331, 3
  %334 = getelementptr i8, ptr %3, i64 %333
  %335 = mul nuw nsw i64 %331, 12
  %336 = getelementptr i8, ptr %3, i64 %335
  %337 = shl nuw nsw i64 %331, 4
  %338 = getelementptr i8, ptr %3, i64 %337
  %339 = mul nuw nsw i64 %331, 20
  %340 = getelementptr i8, ptr %3, i64 %339
  %341 = mul nuw nsw i64 %331, 24
  tail call void @llvm.memset.p0.i64(ptr noundef nonnull align 4 dereferenceable(1) %3, i8 0, i64 %341, i1 false)
  %342 = load float, ptr %0, align 4
  %343 = insertelement <8 x float> poison, float %342, i64 0
  %344 = shufflevector <8 x float> %343, <8 x float> poison, <8 x i32> zeroinitializer
  %345 = getelementptr inbounds nuw i8, ptr %0, i64 4
  %346 = load float, ptr %345, align 4
  %347 = insertelement <8 x float> poison, float %346, i64 0
  %348 = shufflevector <8 x float> %347, <8 x float> poison, <8 x i32> zeroinitializer
  %349 = getelementptr inbounds nuw i8, ptr %0, i64 8
  %350 = load float, ptr %349, align 4
  %351 = insertelement <8 x float> poison, float %350, i64 0
  %352 = shufflevector <8 x float> %351, <8 x float> poison, <8 x i32> zeroinitializer
  %353 = load float, ptr %1, align 4
  %354 = insertelement <8 x float> poison, float %353, i64 0
  %355 = shufflevector <8 x float> %354, <8 x float> poison, <8 x i32> zeroinitializer
  %356 = getelementptr inbounds nuw i8, ptr %1, i64 4
  %357 = load float, ptr %356, align 4
  %358 = insertelement <8 x float> poison, float %357, i64 0
  %359 = shufflevector <8 x float> %358, <8 x float> poison, <8 x i32> zeroinitializer
  %360 = getelementptr inbounds nuw i8, ptr %1, i64 8
  %361 = load float, ptr %360, align 4
  %362 = insertelement <8 x float> poison, float %361, i64 0
  %363 = shufflevector <8 x float> %362, <8 x float> poison, <8 x i32> zeroinitializer
  %364 = zext i32 %8 to i64
  %365 = icmp slt i64 %329, %364
  br i1 %365, label %366, label %748

366:                                              ; preds = %328
  %367 = xor i64 %330, -1
  %368 = icmp ult i32 %7, 8
  %369 = getelementptr i8, ptr %3, i64 %333
  %370 = mul i64 %331, %329
  %371 = shl i64 %370, 2
  %372 = shl nuw nsw i64 %331, 2
  %373 = shl nsw i64 %329, 2
  %374 = add nsw i64 %373, 4
  %375 = mul i64 %331, %374
  %376 = add nsw i64 %330, %329
  %377 = add nsw i64 %376, 4611686018427387903
  %378 = mul i64 %331, %377
  %379 = shl i64 %378, 2
  %380 = mul i64 %331, %376
  %381 = shl i64 %380, 2
  %382 = xor i64 %330, -1
  %383 = add nsw i64 %329, %382
  %384 = mul i64 %331, %383
  %385 = shl i64 %384, 2
  %386 = sub nsw i64 %329, %330
  %387 = shl nsw i64 %386, 2
  %388 = mul i64 %331, %387
  %389 = getelementptr i8, ptr %0, i64 12
  %390 = getelementptr i8, ptr %1, i64 12
  %391 = shl nuw nsw i64 %331, 2
  %392 = getelementptr i8, ptr %3, i64 %391
  %393 = getelementptr i8, ptr %3, i64 %339
  %394 = mul nuw nsw i64 %331, 24
  %395 = getelementptr i8, ptr %3, i64 %394
  %396 = getelementptr i8, ptr %3, i64 %337
  %397 = getelementptr i8, ptr %3, i64 %335
  %398 = getelementptr i8, ptr %5, i64 %371
  %399 = getelementptr i8, ptr %5, i64 %375
  %400 = getelementptr i8, ptr %6, i64 %379
  %401 = getelementptr i8, ptr %6, i64 %381
  %402 = getelementptr i8, ptr %6, i64 %385
  %403 = getelementptr i8, ptr %6, i64 %388
  %404 = icmp ult ptr %0, %336
  %405 = icmp ult ptr %1, %336
  %406 = icmp ult ptr %0, %334
  %407 = icmp ult ptr %1, %334
  %408 = icmp ult ptr %0, %332
  %409 = icmp ult ptr %1, %332
  %410 = icmp ult ptr %0, %395
  %411 = icmp ult ptr %1, %395
  %412 = icmp ult ptr %0, %340
  %413 = icmp ult ptr %1, %340
  %414 = icmp ult ptr %0, %338
  %415 = icmp ult ptr %1, %338
  %416 = and i64 %331, 7
  %417 = icmp eq i64 %416, 0
  br label %418

418:                                              ; preds = %744, %366
  %419 = phi i64 [ %747, %744 ], [ 0, %366 ]
  %420 = phi i64 [ %745, %744 ], [ %329, %366 ]
  %421 = mul i64 %372, %419
  %422 = getelementptr i8, ptr %398, i64 %421
  %423 = getelementptr i8, ptr %399, i64 %421
  %424 = getelementptr i8, ptr %400, i64 %421
  %425 = getelementptr i8, ptr %401, i64 %421
  %426 = getelementptr i8, ptr %402, i64 %421
  %427 = getelementptr i8, ptr %403, i64 %421
  %428 = add i64 %420, %367
  %429 = add nsw i64 %420, %330
  %430 = add nsw i64 %429, -1
  %431 = icmp sgt i64 %428, -1
  %432 = mul i64 %428, %331
  %433 = getelementptr inbounds nuw float, ptr %6, i64 %432
  %434 = icmp samesign ule i64 %429, %364
  %435 = mul i64 %430, %331
  %436 = getelementptr inbounds nuw float, ptr %6, i64 %435
  %437 = icmp sgt i64 %420, -1
  %438 = mul nuw i64 %420, %331
  %439 = getelementptr inbounds nuw float, ptr %5, i64 %438
  br i1 %368, label %442, label %649

440:                                              ; preds = %691
  %441 = icmp samesign ult i64 %650, %331
  br i1 %441, label %442, label %744

442:                                              ; preds = %440, %418
  %443 = phi i64 [ %650, %440 ], [ 0, %418 ]
  %444 = sub nsw i64 %331, %443
  %445 = icmp ult i64 %444, 24
  br i1 %445, label %647, label %446

446:                                              ; preds = %442
  %447 = shl nuw nsw i64 %443, 2
  %448 = getelementptr i8, ptr %369, i64 %447
  %449 = getelementptr i8, ptr %422, i64 %447
  %450 = getelementptr i8, ptr %424, i64 %447
  %451 = getelementptr i8, ptr %426, i64 %447
  %452 = getelementptr i8, ptr %392, i64 %447
  %453 = getelementptr i8, ptr %3, i64 %447
  %454 = getelementptr i8, ptr %393, i64 %447
  %455 = getelementptr i8, ptr %396, i64 %447
  %456 = getelementptr i8, ptr %397, i64 %447
  %457 = icmp ult ptr %448, %423
  %458 = icmp ult ptr %449, %336
  %459 = and i1 %457, %458
  %460 = icmp ult ptr %448, %425
  %461 = icmp ult ptr %450, %336
  %462 = and i1 %460, %461
  %463 = or i1 %459, %462
  %464 = icmp ult ptr %448, %427
  %465 = icmp ult ptr %451, %336
  %466 = and i1 %464, %465
  %467 = or i1 %463, %466
  %468 = icmp ult ptr %448, %389
  %469 = and i1 %468, %404
  %470 = or i1 %467, %469
  %471 = icmp ult ptr %448, %390
  %472 = and i1 %471, %405
  %473 = or i1 %470, %472
  %474 = icmp ult ptr %452, %423
  %475 = icmp ult ptr %449, %334
  %476 = and i1 %474, %475
  %477 = or i1 %473, %476
  %478 = icmp ult ptr %452, %425
  %479 = icmp ult ptr %450, %334
  %480 = and i1 %478, %479
  %481 = or i1 %477, %480
  %482 = icmp ult ptr %452, %427
  %483 = icmp ult ptr %451, %334
  %484 = and i1 %482, %483
  %485 = or i1 %481, %484
  %486 = icmp ult ptr %452, %389
  %487 = and i1 %486, %406
  %488 = or i1 %485, %487
  %489 = icmp ult ptr %452, %390
  %490 = and i1 %489, %407
  %491 = or i1 %488, %490
  %492 = icmp ult ptr %453, %423
  %493 = icmp ult ptr %449, %332
  %494 = and i1 %492, %493
  %495 = or i1 %491, %494
  %496 = icmp ult ptr %453, %425
  %497 = icmp ult ptr %450, %332
  %498 = and i1 %496, %497
  %499 = or i1 %495, %498
  %500 = icmp ult ptr %453, %427
  %501 = icmp ult ptr %451, %332
  %502 = and i1 %500, %501
  %503 = or i1 %499, %502
  %504 = icmp ult ptr %453, %389
  %505 = and i1 %504, %408
  %506 = or i1 %503, %505
  %507 = icmp ult ptr %453, %390
  %508 = and i1 %507, %409
  %509 = or i1 %506, %508
  %510 = icmp ult ptr %454, %423
  %511 = icmp ult ptr %449, %395
  %512 = and i1 %510, %511
  %513 = or i1 %509, %512
  %514 = icmp ult ptr %454, %425
  %515 = icmp ult ptr %450, %395
  %516 = and i1 %514, %515
  %517 = or i1 %513, %516
  %518 = icmp ult ptr %454, %427
  %519 = icmp ult ptr %451, %395
  %520 = and i1 %518, %519
  %521 = or i1 %517, %520
  %522 = icmp ult ptr %454, %389
  %523 = and i1 %522, %410
  %524 = or i1 %521, %523
  %525 = icmp ult ptr %454, %390
  %526 = and i1 %525, %411
  %527 = or i1 %524, %526
  %528 = icmp ult ptr %455, %423
  %529 = icmp ult ptr %449, %340
  %530 = and i1 %528, %529
  %531 = or i1 %527, %530
  %532 = icmp ult ptr %455, %425
  %533 = icmp ult ptr %450, %340
  %534 = and i1 %532, %533
  %535 = or i1 %531, %534
  %536 = icmp ult ptr %455, %427
  %537 = icmp ult ptr %451, %340
  %538 = and i1 %536, %537
  %539 = or i1 %535, %538
  %540 = icmp ult ptr %455, %389
  %541 = and i1 %540, %412
  %542 = or i1 %539, %541
  %543 = icmp ult ptr %455, %390
  %544 = and i1 %543, %413
  %545 = or i1 %542, %544
  %546 = icmp ult ptr %456, %423
  %547 = icmp ult ptr %449, %338
  %548 = and i1 %546, %547
  %549 = or i1 %545, %548
  %550 = icmp ult ptr %456, %425
  %551 = icmp ult ptr %450, %338
  %552 = and i1 %550, %551
  %553 = or i1 %549, %552
  %554 = icmp ult ptr %456, %427
  %555 = icmp ult ptr %451, %338
  %556 = and i1 %554, %555
  %557 = or i1 %553, %556
  %558 = icmp ult ptr %456, %389
  %559 = and i1 %558, %414
  %560 = or i1 %557, %559
  %561 = icmp ult ptr %456, %390
  %562 = and i1 %561, %415
  %563 = or i1 %560, %562
  %564 = icmp ult ptr %449, %425
  %565 = icmp ult ptr %450, %423
  %566 = and i1 %564, %565
  %567 = or i1 %563, %566
  %568 = icmp ult ptr %449, %427
  %569 = icmp ult ptr %451, %423
  %570 = and i1 %568, %569
  %571 = or i1 %567, %570
  %572 = icmp ult ptr %449, %389
  %573 = icmp ult ptr %0, %423
  %574 = and i1 %572, %573
  %575 = or i1 %571, %574
  %576 = icmp ult ptr %449, %390
  %577 = icmp ult ptr %1, %423
  %578 = and i1 %576, %577
  %579 = or i1 %575, %578
  br i1 %579, label %647, label %580

580:                                              ; preds = %446
  %581 = sub i64 %444, %416
  %582 = load float, ptr %360, align 4
  %583 = insertelement <8 x float> poison, float %582, i64 0
  %584 = shufflevector <8 x float> %583, <8 x float> poison, <8 x i32> zeroinitializer
  %585 = load float, ptr %349, align 4
  %586 = insertelement <8 x float> poison, float %585, i64 0
  %587 = shufflevector <8 x float> %586, <8 x float> poison, <8 x i32> zeroinitializer
  %588 = load float, ptr %356, align 4
  %589 = insertelement <8 x float> poison, float %588, i64 0
  %590 = shufflevector <8 x float> %589, <8 x float> poison, <8 x i32> zeroinitializer
  %591 = load float, ptr %345, align 4
  %592 = insertelement <8 x float> poison, float %591, i64 0
  %593 = shufflevector <8 x float> %592, <8 x float> poison, <8 x i32> zeroinitializer
  %594 = load float, ptr %1, align 4
  %595 = insertelement <8 x float> poison, float %594, i64 0
  %596 = shufflevector <8 x float> %595, <8 x float> poison, <8 x i32> zeroinitializer
  %597 = load float, ptr %0, align 4
  %598 = insertelement <8 x float> poison, float %597, i64 0
  %599 = shufflevector <8 x float> %598, <8 x float> poison, <8 x i32> zeroinitializer
  %600 = add i64 %443, %581
  %601 = insertelement <8 x i1> poison, i1 %431, i64 0
  %602 = shufflevector <8 x i1> %601, <8 x i1> poison, <8 x i32> zeroinitializer
  %603 = insertelement <8 x i1> poison, i1 %434, i64 0
  %604 = shufflevector <8 x i1> %603, <8 x i1> poison, <8 x i32> zeroinitializer
  %605 = insertelement <8 x i1> poison, i1 %437, i64 0
  %606 = shufflevector <8 x i1> %605, <8 x i1> poison, <8 x i32> zeroinitializer
  br label %607

607:                                              ; preds = %607, %580
  %608 = phi i64 [ 0, %580 ], [ %644, %607 ]
  %609 = add i64 %443, %608
  %610 = getelementptr float, ptr %433, i64 %609
  %611 = tail call <8 x float> @llvm.masked.load.v8f32.p0(ptr align 4 %610, <8 x i1> %602, <8 x float> poison)
  %612 = select i1 %431, <8 x float> %611, <8 x float> zeroinitializer
  %613 = getelementptr float, ptr %436, i64 %609
  %614 = tail call <8 x float> @llvm.masked.load.v8f32.p0(ptr align 4 %613, <8 x i1> %604, <8 x float> poison)
  %615 = select i1 %434, <8 x float> %614, <8 x float> zeroinitializer
  %616 = fadd <8 x float> %612, %615
  %617 = fmul <8 x float> %599, %616
  %618 = getelementptr inbounds nuw float, ptr %3, i64 %609
  %619 = load <8 x float>, ptr %618, align 4
  %620 = fmul <8 x float> %596, %619
  %621 = fsub <8 x float> %617, %620
  %622 = getelementptr inbounds nuw float, ptr %336, i64 %609
  %623 = load <8 x float>, ptr %622, align 4
  %624 = fsub <8 x float> %621, %623
  %625 = fmul <8 x float> %616, %593
  %626 = getelementptr inbounds nuw float, ptr %332, i64 %609
  %627 = load <8 x float>, ptr %626, align 4
  %628 = fmul <8 x float> %590, %627
  %629 = fsub <8 x float> %625, %628
  %630 = getelementptr inbounds nuw float, ptr %338, i64 %609
  %631 = load <8 x float>, ptr %630, align 4
  %632 = fsub <8 x float> %629, %631
  %633 = fmul <8 x float> %616, %587
  %634 = getelementptr inbounds nuw float, ptr %334, i64 %609
  %635 = load <8 x float>, ptr %634, align 4
  %636 = fmul <8 x float> %584, %635
  %637 = fsub <8 x float> %633, %636
  %638 = getelementptr inbounds nuw float, ptr %340, i64 %609
  %639 = load <8 x float>, ptr %638, align 4
  %640 = fsub <8 x float> %637, %639
  store <8 x float> %619, ptr %622, align 4
  store <8 x float> %627, ptr %630, align 4
  store <8 x float> %635, ptr %638, align 4
  store <8 x float> %624, ptr %618, align 4
  store <8 x float> %632, ptr %626, align 4
  store <8 x float> %640, ptr %634, align 4
  %641 = fadd <8 x float> %624, %632
  %642 = fadd <8 x float> %641, %640
  %643 = getelementptr float, ptr %439, i64 %609
  tail call void @llvm.masked.store.v8f32.p0(<8 x float> %642, ptr align 4 %643, <8 x i1> %606)
  %644 = add nuw i64 %608, 8
  %645 = icmp eq i64 %644, %581
  br i1 %645, label %646, label %607, !llvm.loop !69

646:                                              ; preds = %607
  br i1 %417, label %744, label %647

647:                                              ; preds = %446, %442, %646
  %648 = phi i64 [ %443, %446 ], [ %443, %442 ], [ %600, %646 ]
  br label %694

649:                                              ; preds = %418, %691
  %650 = phi i64 [ %692, %691 ], [ 8, %418 ]
  %651 = phi i64 [ %650, %691 ], [ 0, %418 ]
  br i1 %431, label %652, label %655

652:                                              ; preds = %649
  %653 = getelementptr inbounds nuw float, ptr %433, i64 %651
  %654 = load <8 x float>, ptr %653, align 1
  br label %655

655:                                              ; preds = %652, %649
  %656 = phi <8 x float> [ %654, %652 ], [ zeroinitializer, %649 ]
  br i1 %434, label %657, label %660

657:                                              ; preds = %655
  %658 = getelementptr inbounds nuw float, ptr %436, i64 %651
  %659 = load <8 x float>, ptr %658, align 1
  br label %660

660:                                              ; preds = %657, %655
  %661 = phi <8 x float> [ %659, %657 ], [ zeroinitializer, %655 ]
  %662 = fadd <8 x float> %656, %661
  %663 = getelementptr inbounds nuw float, ptr %3, i64 %651
  %664 = load <8 x float>, ptr %663, align 1
  %665 = getelementptr inbounds nuw float, ptr %332, i64 %651
  %666 = load <8 x float>, ptr %665, align 1
  %667 = getelementptr inbounds nuw float, ptr %334, i64 %651
  %668 = load <8 x float>, ptr %667, align 1
  %669 = getelementptr inbounds nuw float, ptr %336, i64 %651
  %670 = load <8 x float>, ptr %669, align 1
  %671 = getelementptr inbounds nuw float, ptr %338, i64 %651
  %672 = load <8 x float>, ptr %671, align 1
  %673 = getelementptr inbounds nuw float, ptr %340, i64 %651
  %674 = load <8 x float>, ptr %673, align 1
  %675 = fmul <8 x float> %344, %662
  %676 = fmul <8 x float> %355, %664
  %677 = fsub <8 x float> %675, %676
  %678 = fsub <8 x float> %677, %670
  %679 = fmul <8 x float> %348, %662
  %680 = fmul <8 x float> %359, %666
  %681 = fsub <8 x float> %679, %680
  %682 = fsub <8 x float> %681, %672
  %683 = fmul <8 x float> %352, %662
  %684 = fmul <8 x float> %363, %668
  %685 = fsub <8 x float> %683, %684
  %686 = fsub <8 x float> %685, %674
  store <8 x float> %664, ptr %669, align 1
  store <8 x float> %666, ptr %671, align 1
  store <8 x float> %668, ptr %673, align 1
  store <8 x float> %678, ptr %663, align 1
  store <8 x float> %682, ptr %665, align 1
  store <8 x float> %686, ptr %667, align 1
  br i1 %437, label %687, label %691

687:                                              ; preds = %660
  %688 = fadd <8 x float> %678, %682
  %689 = fadd <8 x float> %688, %686
  %690 = getelementptr inbounds nuw float, ptr %439, i64 %651
  store <8 x float> %689, ptr %690, align 1
  br label %691

691:                                              ; preds = %687, %660
  %692 = add nuw nsw i64 %650, 8
  %693 = icmp samesign ugt i64 %692, %331
  br i1 %693, label %440, label %649, !llvm.loop !70

694:                                              ; preds = %647, %741
  %695 = phi i64 [ %742, %741 ], [ %648, %647 ]
  br i1 %431, label %696, label %699

696:                                              ; preds = %694
  %697 = getelementptr inbounds nuw float, ptr %433, i64 %695
  %698 = load float, ptr %697, align 4
  br label %699

699:                                              ; preds = %696, %694
  %700 = phi float [ %698, %696 ], [ 0.000000e+00, %694 ]
  br i1 %434, label %701, label %704

701:                                              ; preds = %699
  %702 = getelementptr inbounds nuw float, ptr %436, i64 %695
  %703 = load float, ptr %702, align 4
  br label %704

704:                                              ; preds = %701, %699
  %705 = phi float [ %703, %701 ], [ 0.000000e+00, %699 ]
  %706 = fadd float %700, %705
  %707 = load float, ptr %0, align 4
  %708 = fmul float %707, %706
  %709 = load float, ptr %1, align 4
  %710 = getelementptr inbounds nuw float, ptr %3, i64 %695
  %711 = load float, ptr %710, align 4
  %712 = fmul float %709, %711
  %713 = fsub float %708, %712
  %714 = getelementptr inbounds nuw float, ptr %336, i64 %695
  %715 = load float, ptr %714, align 4
  %716 = fsub float %713, %715
  %717 = load float, ptr %345, align 4
  %718 = fmul float %706, %717
  %719 = load float, ptr %356, align 4
  %720 = getelementptr inbounds nuw float, ptr %332, i64 %695
  %721 = load float, ptr %720, align 4
  %722 = fmul float %719, %721
  %723 = fsub float %718, %722
  %724 = getelementptr inbounds nuw float, ptr %338, i64 %695
  %725 = load float, ptr %724, align 4
  %726 = fsub float %723, %725
  %727 = load float, ptr %349, align 4
  %728 = fmul float %706, %727
  %729 = load float, ptr %360, align 4
  %730 = getelementptr inbounds nuw float, ptr %334, i64 %695
  %731 = load float, ptr %730, align 4
  %732 = fmul float %729, %731
  %733 = fsub float %728, %732
  %734 = getelementptr inbounds nuw float, ptr %340, i64 %695
  %735 = load float, ptr %734, align 4
  %736 = fsub float %733, %735
  store float %711, ptr %714, align 4
  store float %721, ptr %724, align 4
  store float %731, ptr %734, align 4
  store float %716, ptr %710, align 4
  store float %726, ptr %720, align 4
  store float %736, ptr %730, align 4
  br i1 %437, label %737, label %741

737:                                              ; preds = %704
  %738 = fadd float %716, %726
  %739 = fadd float %738, %736
  %740 = getelementptr inbounds nuw float, ptr %439, i64 %695
  store float %739, ptr %740, align 4
  br label %741

741:                                              ; preds = %737, %704
  %742 = add nuw nsw i64 %695, 1
  %743 = icmp eq i64 %742, %331
  br i1 %743, label %744, label %694, !llvm.loop !71

744:                                              ; preds = %741, %646, %440
  %745 = add nsw i64 %420, 1
  %746 = icmp eq i64 %745, %364
  %747 = add i64 %419, 1
  br i1 %746, label %748, label %418, !llvm.loop !72

748:                                              ; preds = %744, %328
  ret void
}
