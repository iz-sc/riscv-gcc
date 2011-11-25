/* Test __atomic routines for existence and proper execution on 8 byte 
   values with each valid memory model.  */
/* { dg-do run } */
/* { dg-require-effective-target sync_long_long_runtime } */
/* { dg-options "" } */

/* Test the execution of the __atomic_store_n builtin for a long long.  */

extern void abort(void);

long long v, count;

main ()
{
  v = 0;
  count = 0;

  __atomic_store_n (&v, count + 1, __ATOMIC_RELAXED);
  if (v != ++count)
    abort ();

  __atomic_store_n (&v, count + 1, __ATOMIC_RELEASE);
  if (v != ++count)
    abort ();

  __atomic_store_n (&v, count + 1, __ATOMIC_SEQ_CST);
  if (v != ++count)
    abort ();

  /* Now test the generic variant.  */
  count++;

  __atomic_store (&v, &count, __ATOMIC_RELAXED);
  if (v != count++)
    abort ();

  __atomic_store (&v, &count, __ATOMIC_RELEASE);
  if (v != count++)
    abort ();

  __atomic_store (&v, &count, __ATOMIC_SEQ_CST);
  if (v != count)
    abort ();


  return 0;
}

