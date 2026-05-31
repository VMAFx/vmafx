define dso_local void @od_bin_fdct8x8_avx2(ptr noundef writeonly captures(address_is_null) %0, i32 noundef %1, ptr noundef readonly captures(address_is_null) %2, i32 noundef %3) local_unnamed_addr {
  %5 = icmp eq ptr %0, null
  br i1 %5, label %6, label %7

6:                                                ; preds = %4
  tail call void @__assert_fail(ptr noundef nonnull @.str, ptr noundef nonnull @.str.1, i32 noundef 253, ptr noundef nonnull @__PRETTY_FUNCTION__.od_bin_fdct8x8_avx2)
  unreachable

7:                                                ; preds = %4
  %8 = icmp eq ptr %2, null
  br i1 %8, label %9, label %10

9:                                                ; preds = %7
  tail call void @__assert_fail(ptr noundef nonnull @.str.2, ptr noundef nonnull @.str.1, i32 noundef 254, ptr noundef nonnull @__PRETTY_FUNCTION__.od_bin_fdct8x8_avx2)
  unreachable

10:                                               ; preds = %7
  %11 = icmp sgt i32 %3, 7
  br i1 %11, label %13, label %12

12:                                               ; preds = %10
  tail call void @__assert_fail(ptr noundef nonnull @.str.3, ptr noundef nonnull @.str.1, i32 noundef 255, ptr noundef nonnull @__PRETTY_FUNCTION__.od_bin_fdct8x8_avx2)
  unreachable

13:                                               ; preds = %10
  %14 = icmp sgt i32 %1, 7
  br i1 %14, label %16, label %15

15:                                               ; preds = %13
  tail call void @__assert_fail(ptr noundef nonnull @.str.4, ptr noundef nonnull @.str.1, i32 noundef 256, ptr noundef nonnull @__PRETTY_FUNCTION__.od_bin_fdct8x8_avx2)
  unreachable

16:                                               ; preds = %13
  %17 = zext nneg i32 %3 to i64
  %18 = zext nneg i32 %1 to i64
  %19 = load <8 x i32>, ptr %2, align 1
  %20 = getelementptr inbounds nuw i32, ptr %2, i64 %17
  %21 = load <8 x i32>, ptr %20, align 1
  %22 = shl nuw nsw i64 %17, 3
  %23 = getelementptr inbounds nuw i8, ptr %2, i64 %22
  %24 = load <8 x i32>, ptr %23, align 1
  %25 = mul nuw nsw i64 %17, 12
  %26 = getelementptr inbounds nuw i8, ptr %2, i64 %25
  %27 = load <8 x i32>, ptr %26, align 1
  %28 = shl nuw nsw i64 %17, 4
  %29 = getelementptr inbounds nuw i8, ptr %2, i64 %28
  %30 = load <8 x i32>, ptr %29, align 1
  %31 = mul nuw nsw i64 %17, 20
  %32 = getelementptr inbounds nuw i8, ptr %2, i64 %31
  %33 = load <8 x i32>, ptr %32, align 1
  %34 = mul nuw nsw i64 %17, 24
  %35 = getelementptr inbounds nuw i8, ptr %2, i64 %34
  %36 = load <8 x i32>, ptr %35, align 1
  %37 = mul nuw nsw i64 %17, 28
  %38 = getelementptr inbounds nuw i8, ptr %2, i64 %37
  %39 = load <8 x i32>, ptr %38, align 1
  %40 = sub <8 x i32> %19, %39
  %41 = lshr <8 x i32> %40, splat (i32 31)
  %42 = add <8 x i32> %41, %40
  %43 = ashr <8 x i32> %42, splat (i32 1)
  %44 = sub <8 x i32> %19, %43
  %45 = add <8 x i32> %36, %21
  %46 = lshr <8 x i32> %45, splat (i32 31)
  %47 = add <8 x i32> %46, %45
  %48 = ashr <8 x i32> %47, splat (i32 1)
  %49 = sub <8 x i32> %36, %48
  %50 = sub <8 x i32> %24, %33
  %51 = lshr <8 x i32> %50, splat (i32 31)
  %52 = add <8 x i32> %51, %50
  %53 = ashr <8 x i32> %52, splat (i32 1)
  %54 = sub <8 x i32> %53, %24
  %55 = add <8 x i32> %30, %27
  %56 = lshr <8 x i32> %55, splat (i32 31)
  %57 = add <8 x i32> %56, %55
  %58 = ashr <8 x i32> %57, splat (i32 1)
  %59 = sub <8 x i32> %30, %58
  %60 = add <8 x i32> %44, %58
  %61 = add <8 x i32> %54, %48
  %62 = sub <8 x i32> %61, %45
  %63 = mul <8 x i32> %62, splat (i32 13573)
  %64 = add <8 x i32> %63, splat (i32 16384)
  %65 = ashr <8 x i32> %64, splat (i32 15)
  %66 = sub <8 x i32> %60, %65
  %67 = mul <8 x i32> %66, splat (i32 11585)
  %68 = add <8 x i32> %67, splat (i32 8192)
  %69 = ashr <8 x i32> %68, splat (i32 14)
  %70 = add <8 x i32> %69, %62
  %71 = mul <8 x i32> %70, splat (i32 13573)
  %72 = add <8 x i32> %71, splat (i32 16384)
  %73 = ashr <8 x i32> %72, splat (i32 15)
  %74 = sub <8 x i32> %66, %73
  %75 = mul <8 x i32> %61, splat (i32 21895)
  %76 = add <8 x i32> %75, splat (i32 16384)
  %77 = ashr <8 x i32> %76, splat (i32 15)
  %78 = add <8 x i32> %55, %77
  %79 = sub <8 x i32> %60, %78
  %80 = mul <8 x i32> %79, splat (i32 15137)
  %81 = add <8 x i32> %80, splat (i32 8192)
  %82 = ashr <8 x i32> %81, splat (i32 14)
  %83 = add <8 x i32> %82, %61
  %84 = mul <8 x i32> %83, splat (i32 21895)
  %85 = add <8 x i32> %84, splat (i32 16384)
  %86 = ashr <8 x i32> %85, splat (i32 15)
  %87 = sub <8 x i32> %79, %86
  %88 = mul <8 x i32> %49, splat (i32 19195)
  %89 = add <8 x i32> %88, splat (i32 16384)
  %90 = ashr <8 x i32> %89, splat (i32 15)
  %91 = add <8 x i32> %90, %50
  %92 = mul <8 x i32> %91, splat (i32 11585)
  %93 = add <8 x i32> %92, splat (i32 8192)
  %94 = ashr <8 x i32> %93, splat (i32 14)
  %95 = add <8 x i32> %94, %49
  %96 = mul <8 x i32> %95, splat (i32 7489)
  %97 = add <8 x i32> %96, splat (i32 4096)
  %98 = ashr <8 x i32> %97, splat (i32 13)
  %99 = sub <8 x i32> %98, %91
  %100 = lshr <8 x i32> %95, splat (i32 31)
  %101 = add <8 x i32> %100, %95
  %102 = ashr <8 x i32> %101, splat (i32 1)
  %103 = add <8 x i32> %102, %59
  %104 = sub <8 x i32> %95, %103
  %105 = add <8 x i32> %99, %43
  %106 = sub <8 x i32> %40, %105
  %107 = mul <8 x i32> %106, splat (i32 3227)
  %108 = add <8 x i32> %107, splat (i32 16384)
  %109 = ashr <8 x i32> %108, splat (i32 15)
  %110 = add <8 x i32> %109, %103
  %111 = mul <8 x i32> %110, splat (i32 6393)
  %112 = add <8 x i32> %111, splat (i32 16384)
  %113 = ashr <8 x i32> %112, splat (i32 15)
  %114 = sub <8 x i32> %106, %113
  %115 = mul <8 x i32> %114, splat (i32 3227)
  %116 = add <8 x i32> %115, splat (i32 16384)
  %117 = ashr <8 x i32> %116, splat (i32 15)
  %118 = add <8 x i32> %117, %110
  %119 = mul <8 x i32> %105, splat (i32 2485)
  %120 = add <8 x i32> %119, splat (i32 4096)
  %121 = ashr <8 x i32> %120, splat (i32 13)
  %122 = add <8 x i32> %121, %104
  %123 = mul <8 x i32> %122, splat (i32 18205)
  %124 = add <8 x i32> %123, splat (i32 16384)
  %125 = ashr <8 x i32> %124, splat (i32 15)
  %126 = sub <8 x i32> %105, %125
  %127 = mul <8 x i32> %126, splat (i32 2485)
  %128 = add <8 x i32> %127, splat (i32 4096)
  %129 = ashr <8 x i32> %128, splat (i32 13)
  %130 = add <8 x i32> %129, %122
  %131 = shufflevector <8 x i32> %74, <8 x i32> %114, <8 x i32> <i32 0, i32 8, i32 1, i32 9, i32 4, i32 12, i32 5, i32 13>
  %132 = shufflevector <8 x i32> %74, <8 x i32> %114, <8 x i32> <i32 2, i32 10, i32 3, i32 11, i32 6, i32 14, i32 7, i32 15>
  %133 = shufflevector <8 x i32> %83, <8 x i32> %126, <8 x i32> <i32 0, i32 8, i32 1, i32 9, i32 4, i32 12, i32 5, i32 13>
  %134 = shufflevector <8 x i32> %83, <8 x i32> %126, <8 x i32> <i32 2, i32 10, i32 3, i32 11, i32 6, i32 14, i32 7, i32 15>
  %135 = shufflevector <8 x i32> %70, <8 x i32> %130, <8 x i32> <i32 0, i32 8, i32 1, i32 9, i32 4, i32 12, i32 5, i32 13>
  %136 = shufflevector <8 x i32> %70, <8 x i32> %130, <8 x i32> <i32 2, i32 10, i32 3, i32 11, i32 6, i32 14, i32 7, i32 15>
  %137 = shufflevector <8 x i32> %87, <8 x i32> %118, <8 x i32> <i32 0, i32 8, i32 1, i32 9, i32 4, i32 12, i32 5, i32 13>
  %138 = shufflevector <8 x i32> %87, <8 x i32> %118, <8 x i32> <i32 2, i32 10, i32 3, i32 11, i32 6, i32 14, i32 7, i32 15>
  %139 = shufflevector <8 x i32> %131, <8 x i32> %133, <8 x i32> <i32 0, i32 1, i32 8, i32 9, i32 4, i32 5, i32 12, i32 13>
  %140 = shufflevector <8 x i32> %131, <8 x i32> %133, <8 x i32> <i32 2, i32 3, i32 10, i32 11, i32 6, i32 7, i32 14, i32 15>
  %141 = shufflevector <8 x i32> %132, <8 x i32> %134, <8 x i32> <i32 0, i32 1, i32 8, i32 9, i32 4, i32 5, i32 12, i32 13>
  %142 = shufflevector <8 x i32> %132, <8 x i32> %134, <8 x i32> <i32 2, i32 3, i32 10, i32 11, i32 6, i32 7, i32 14, i32 15>
  %143 = shufflevector <8 x i32> %135, <8 x i32> %137, <8 x i32> <i32 0, i32 1, i32 8, i32 9, i32 4, i32 5, i32 12, i32 13>
  %144 = shufflevector <8 x i32> %135, <8 x i32> %137, <8 x i32> <i32 2, i32 3, i32 10, i32 11, i32 6, i32 7, i32 14, i32 15>
  %145 = shufflevector <8 x i32> %136, <8 x i32> %138, <8 x i32> <i32 0, i32 1, i32 8, i32 9, i32 4, i32 5, i32 12, i32 13>
  %146 = shufflevector <8 x i32> %136, <8 x i32> %138, <8 x i32> <i32 2, i32 3, i32 10, i32 11, i32 6, i32 7, i32 14, i32 15>
  %147 = shufflevector <8 x i32> %139, <8 x i32> %143, <8 x i32> <i32 0, i32 1, i32 2, i32 3, i32 8, i32 9, i32 10, i32 11>
  %148 = shufflevector <8 x i32> %140, <8 x i32> %144, <8 x i32> <i32 0, i32 1, i32 2, i32 3, i32 8, i32 9, i32 10, i32 11>
  %149 = shufflevector <8 x i32> %141, <8 x i32> %145, <8 x i32> <i32 0, i32 1, i32 2, i32 3, i32 8, i32 9, i32 10, i32 11>
  %150 = shufflevector <8 x i32> %142, <8 x i32> %146, <8 x i32> <i32 0, i32 1, i32 2, i32 3, i32 8, i32 9, i32 10, i32 11>
  %151 = shufflevector <8 x i32> %139, <8 x i32> %143, <8 x i32> <i32 4, i32 5, i32 6, i32 7, i32 12, i32 13, i32 14, i32 15>
  %152 = shufflevector <8 x i32> %140, <8 x i32> %144, <8 x i32> <i32 4, i32 5, i32 6, i32 7, i32 12, i32 13, i32 14, i32 15>
  %153 = shufflevector <8 x i32> %141, <8 x i32> %145, <8 x i32> <i32 4, i32 5, i32 6, i32 7, i32 12, i32 13, i32 14, i32 15>
  %154 = shufflevector <8 x i32> %142, <8 x i32> %146, <8 x i32> <i32 4, i32 5, i32 6, i32 7, i32 12, i32 13, i32 14, i32 15>
  %155 = sub <8 x i32> %147, %154
  %156 = lshr <8 x i32> %155, splat (i32 31)
  %157 = add <8 x i32> %156, %155
  %158 = ashr <8 x i32> %157, splat (i32 1)
  %159 = sub <8 x i32> %147, %158
  %160 = add <8 x i32> %153, %148
  %161 = lshr <8 x i32> %160, splat (i32 31)
  %162 = add <8 x i32> %161, %160
  %163 = ashr <8 x i32> %162, splat (i32 1)
  %164 = sub <8 x i32> %153, %163
  %165 = sub <8 x i32> %149, %152
  %166 = lshr <8 x i32> %165, splat (i32 31)
  %167 = add <8 x i32> %166, %165
  %168 = ashr <8 x i32> %167, splat (i32 1)
  %169 = sub <8 x i32> %168, %149
  %170 = add <8 x i32> %151, %150
  %171 = lshr <8 x i32> %170, splat (i32 31)
  %172 = add <8 x i32> %171, %170
  %173 = ashr <8 x i32> %172, splat (i32 1)
  %174 = sub <8 x i32> %151, %173
  %175 = add <8 x i32> %159, %173
  %176 = add <8 x i32> %169, %163
  %177 = sub <8 x i32> %176, %160
  %178 = mul <8 x i32> %177, splat (i32 13573)
  %179 = add <8 x i32> %178, splat (i32 16384)
  %180 = ashr <8 x i32> %179, splat (i32 15)
  %181 = sub <8 x i32> %175, %180
  %182 = mul <8 x i32> %181, splat (i32 11585)
  %183 = add <8 x i32> %182, splat (i32 8192)
  %184 = ashr <8 x i32> %183, splat (i32 14)
  %185 = add <8 x i32> %184, %177
  %186 = mul <8 x i32> %185, splat (i32 13573)
  %187 = add <8 x i32> %186, splat (i32 16384)
  %188 = ashr <8 x i32> %187, splat (i32 15)
  %189 = sub <8 x i32> %181, %188
  %190 = mul <8 x i32> %176, splat (i32 21895)
  %191 = add <8 x i32> %190, splat (i32 16384)
  %192 = ashr <8 x i32> %191, splat (i32 15)
  %193 = add <8 x i32> %170, %192
  %194 = sub <8 x i32> %175, %193
  %195 = mul <8 x i32> %194, splat (i32 15137)
  %196 = add <8 x i32> %195, splat (i32 8192)
  %197 = ashr <8 x i32> %196, splat (i32 14)
  %198 = add <8 x i32> %197, %176
  %199 = mul <8 x i32> %198, splat (i32 21895)
  %200 = add <8 x i32> %199, splat (i32 16384)
  %201 = ashr <8 x i32> %200, splat (i32 15)
  %202 = sub <8 x i32> %194, %201
  %203 = mul <8 x i32> %164, splat (i32 19195)
  %204 = add <8 x i32> %203, splat (i32 16384)
  %205 = ashr <8 x i32> %204, splat (i32 15)
  %206 = add <8 x i32> %205, %165
  %207 = mul <8 x i32> %206, splat (i32 11585)
  %208 = add <8 x i32> %207, splat (i32 8192)
  %209 = ashr <8 x i32> %208, splat (i32 14)
  %210 = add <8 x i32> %209, %164
  %211 = mul <8 x i32> %210, splat (i32 7489)
  %212 = add <8 x i32> %211, splat (i32 4096)
  %213 = ashr <8 x i32> %212, splat (i32 13)
  %214 = sub <8 x i32> %213, %206
  %215 = lshr <8 x i32> %210, splat (i32 31)
  %216 = add <8 x i32> %215, %210
  %217 = ashr <8 x i32> %216, splat (i32 1)
  %218 = add <8 x i32> %217, %174
  %219 = sub <8 x i32> %210, %218
  %220 = add <8 x i32> %214, %158
  %221 = sub <8 x i32> %155, %220
  %222 = mul <8 x i32> %221, splat (i32 3227)
  %223 = add <8 x i32> %222, splat (i32 16384)
  %224 = ashr <8 x i32> %223, splat (i32 15)
  %225 = add <8 x i32> %224, %218
  %226 = mul <8 x i32> %225, splat (i32 6393)
  %227 = add <8 x i32> %226, splat (i32 16384)
  %228 = ashr <8 x i32> %227, splat (i32 15)
  %229 = sub <8 x i32> %221, %228
  %230 = mul <8 x i32> %229, splat (i32 3227)
  %231 = add <8 x i32> %230, splat (i32 16384)
  %232 = ashr <8 x i32> %231, splat (i32 15)
  %233 = add <8 x i32> %232, %225
  %234 = mul <8 x i32> %220, splat (i32 2485)
  %235 = add <8 x i32> %234, splat (i32 4096)
  %236 = ashr <8 x i32> %235, splat (i32 13)
  %237 = add <8 x i32> %236, %219
  %238 = mul <8 x i32> %237, splat (i32 18205)
  %239 = add <8 x i32> %238, splat (i32 16384)
  %240 = ashr <8 x i32> %239, splat (i32 15)
  %241 = sub <8 x i32> %220, %240
  %242 = mul <8 x i32> %241, splat (i32 2485)
  %243 = add <8 x i32> %242, splat (i32 4096)
  %244 = ashr <8 x i32> %243, splat (i32 13)
  %245 = add <8 x i32> %244, %237
  %246 = shufflevector <8 x i32> %189, <8 x i32> %229, <8 x i32> <i32 0, i32 8, i32 1, i32 9, i32 4, i32 12, i32 5, i32 13>
  %247 = shufflevector <8 x i32> %189, <8 x i32> %229, <8 x i32> <i32 2, i32 10, i32 3, i32 11, i32 6, i32 14, i32 7, i32 15>
  %248 = shufflevector <8 x i32> %198, <8 x i32> %241, <8 x i32> <i32 0, i32 8, i32 1, i32 9, i32 4, i32 12, i32 5, i32 13>
  %249 = shufflevector <8 x i32> %198, <8 x i32> %241, <8 x i32> <i32 2, i32 10, i32 3, i32 11, i32 6, i32 14, i32 7, i32 15>
  %250 = shufflevector <8 x i32> %185, <8 x i32> %245, <8 x i32> <i32 0, i32 8, i32 1, i32 9, i32 4, i32 12, i32 5, i32 13>
  %251 = shufflevector <8 x i32> %185, <8 x i32> %245, <8 x i32> <i32 2, i32 10, i32 3, i32 11, i32 6, i32 14, i32 7, i32 15>
  %252 = shufflevector <8 x i32> %202, <8 x i32> %233, <8 x i32> <i32 0, i32 8, i32 1, i32 9, i32 4, i32 12, i32 5, i32 13>
  %253 = shufflevector <8 x i32> %202, <8 x i32> %233, <8 x i32> <i32 2, i32 10, i32 3, i32 11, i32 6, i32 14, i32 7, i32 15>
  %254 = shufflevector <8 x i32> %246, <8 x i32> %248, <8 x i32> <i32 0, i32 1, i32 8, i32 9, i32 4, i32 5, i32 12, i32 13>
  %255 = shufflevector <8 x i32> %246, <8 x i32> %248, <8 x i32> <i32 2, i32 3, i32 10, i32 11, i32 6, i32 7, i32 14, i32 15>
  %256 = shufflevector <8 x i32> %247, <8 x i32> %249, <8 x i32> <i32 0, i32 1, i32 8, i32 9, i32 4, i32 5, i32 12, i32 13>
  %257 = shufflevector <8 x i32> %247, <8 x i32> %249, <8 x i32> <i32 2, i32 3, i32 10, i32 11, i32 6, i32 7, i32 14, i32 15>
  %258 = shufflevector <8 x i32> %250, <8 x i32> %252, <8 x i32> <i32 0, i32 1, i32 8, i32 9, i32 4, i32 5, i32 12, i32 13>
  %259 = shufflevector <8 x i32> %250, <8 x i32> %252, <8 x i32> <i32 2, i32 3, i32 10, i32 11, i32 6, i32 7, i32 14, i32 15>
  %260 = shufflevector <8 x i32> %251, <8 x i32> %253, <8 x i32> <i32 0, i32 1, i32 8, i32 9, i32 4, i32 5, i32 12, i32 13>
  %261 = shufflevector <8 x i32> %251, <8 x i32> %253, <8 x i32> <i32 2, i32 3, i32 10, i32 11, i32 6, i32 7, i32 14, i32 15>
  %262 = shufflevector <8 x i32> %254, <8 x i32> %258, <8 x i32> <i32 0, i32 1, i32 2, i32 3, i32 8, i32 9, i32 10, i32 11>
  %263 = shufflevector <8 x i32> %255, <8 x i32> %259, <8 x i32> <i32 0, i32 1, i32 2, i32 3, i32 8, i32 9, i32 10, i32 11>
  %264 = shufflevector <8 x i32> %256, <8 x i32> %260, <8 x i32> <i32 0, i32 1, i32 2, i32 3, i32 8, i32 9, i32 10, i32 11>
  %265 = shufflevector <8 x i32> %257, <8 x i32> %261, <8 x i32> <i32 0, i32 1, i32 2, i32 3, i32 8, i32 9, i32 10, i32 11>
  %266 = shufflevector <8 x i32> %254, <8 x i32> %258, <8 x i32> <i32 4, i32 5, i32 6, i32 7, i32 12, i32 13, i32 14, i32 15>
  %267 = shufflevector <8 x i32> %255, <8 x i32> %259, <8 x i32> <i32 4, i32 5, i32 6, i32 7, i32 12, i32 13, i32 14, i32 15>
  %268 = shufflevector <8 x i32> %256, <8 x i32> %260, <8 x i32> <i32 4, i32 5, i32 6, i32 7, i32 12, i32 13, i32 14, i32 15>
  %269 = shufflevector <8 x i32> %257, <8 x i32> %261, <8 x i32> <i32 4, i32 5, i32 6, i32 7, i32 12, i32 13, i32 14, i32 15>
  store <8 x i32> %262, ptr %0, align 1
  %270 = getelementptr inbounds nuw i32, ptr %0, i64 %18
  store <8 x i32> %263, ptr %270, align 1
  %271 = shl nuw nsw i64 %18, 3
  %272 = getelementptr inbounds nuw i8, ptr %0, i64 %271
  store <8 x i32> %264, ptr %272, align 1
  %273 = mul nuw nsw i64 %18, 12
  %274 = getelementptr inbounds nuw i8, ptr %0, i64 %273
  store <8 x i32> %265, ptr %274, align 1
  %275 = shl nuw nsw i64 %18, 4
  %276 = getelementptr inbounds nuw i8, ptr %0, i64 %275
  store <8 x i32> %266, ptr %276, align 1
  %277 = mul nuw nsw i64 %18, 20
  %278 = getelementptr inbounds nuw i8, ptr %0, i64 %277
  store <8 x i32> %267, ptr %278, align 1
  %279 = mul nuw nsw i64 %18, 24
  %280 = getelementptr inbounds nuw i8, ptr %0, i64 %279
  store <8 x i32> %268, ptr %280, align 1
  %281 = mul nuw nsw i64 %18, 28
  %282 = getelementptr inbounds nuw i8, ptr %0, i64 %281
  store <8 x i32> %269, ptr %282, align 1
  ret void
}
