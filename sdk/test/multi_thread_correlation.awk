#
#Test to match up sycl runtime piEnqueueKernelLaunch function records with L0 kernel view records, via correlation id.
#  -- checks that matched pairs on correlation_id and kernel_id have same threadid.
#
#To Run:
#  <build_directory>/samples/dpc_gemm_threaded/dpc_gemm_threaded > tst.out
#  awk -f <build_directory>/../test/multi_thread_correlation.awk tst.out
#
#  TBD -- auto integrate me into googletest if possible
#
BEGIN {
        tid = 0;
}

/Sycl Thread Id:/ { tid=$4 };
/Sycl Start Time:/ { sycl_start_ts = $4 };
/Sycl End Time:/ { sycl_end_ts = $4 };
/^Sycl Correlation Id:/ && $4 > 0 {
	sycl[$4]= tid;
	sycl_start_time[$4]= sycl_start_ts;
	sycl_end_time[$4]= sycl_end_ts;
	sycl_line_number[$4]= NR;
       	};

/Correlation Id :/ { kid=$4 };
/Ze Kernel Append Time:/ { append_time=$5 };
/Ze Kernel Submit Time:/ { submit_time=$5 };
/Ze Kernel Start Time:/ { start_time=$5 };
/Ze Kernel End Time:/ { end_time=$5 };
/Kernel Thread Id :/ {
	kthreads[kid]=$5
	k_type[kid]="Kernel"
	k_append_time[kid]=append_time;
	k_submit_time[kid]=submit_time;
	k_start_time[kid]=start_time;
	k_end_time[kid]=end_time;
	k_line_number[kid]= NR;
	};

/Memory Op Id :/ { mid=$5 };
/Memory Op Append Time:/ { append_time=$5 };
/Memory Op Submit Time:/ { submit_time=$5 };
/Memory Op Start Time:/ { start_time=$5 };
/Memory Op End Time:/ { end_time=$5 };
/Memory Op Thread Id :/ {
	kthreads[mid]=$6
	k_type[mid]="Memory"
	k_append_time[mid]=append_time;
	k_submit_time[mid]=submit_time;
	k_start_time[mid]=start_time;
	k_end_time[mid]=end_time;
	k_line_number[mid]= NR;
	};

END {
	#if (length(sycl) != length(kthreads)) {
		#print ("Correlation Id issues --- lengths don't match: ",length(sycl),":",length(kthreads))
		#for (corrId in kthreads) if (kthreads[corrId] != sycl[corrId]) print(corrId);
		#};
	for (corrId in kthreads) {
		if (sycl[corrId] != kthreads[corrId]) {
		  print(k_type[corrId]);
		  print ("Correlation Id issues --- Threads don't match: ", sycl[corrId], kthreads[corrId], k_type[corrId])
                  print("------------>for correlationId:", corrId, "- sycl line number: ",sycl_line_number[corrId],"- levelZero line number:",k_line_number[corrId]);
		  corrErr=1;
		};
		if (! (sycl_start_time[corrId] <= k_append_time[corrId] &&
                  sycl_end_time[corrId] >= sycl_start_time[corrId] &&
                  k_append_time[corrId] <= k_submit_time[corrId] &&
                  k_submit_time[corrId] <= k_start_time[corrId] &&
                  k_start_time[corrId] <= k_end_time[corrId]))
		 {
		  corrErr=1;
                  print("------------>Not All monotonic for correlationId:", corrId, "- sycl line number: ",sycl_line_number[corrId],"- levelZero line number:",k_line_number[corrId]);
		  print("Type: ",k_type[corrId]);
		  print("Sycl start time:    ",sycl_start_time[corrId]);
		  print("append time: ",k_append_time[corrId]);
		  print("submit time: ",k_submit_time[corrId]);
		  print("start time:  ",k_start_time[corrId]);
		  print("end time:    ",k_end_time[corrId]);
		  print("Sycl end time:      ",sycl_end_time[corrId]);
		}
	}
	if (!corrErr) {
		print("All correlation Ids match Kernel Id Threads");
		exit 0
	}
	else  {
		exit 1
	}
}
