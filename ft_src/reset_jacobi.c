#include <stdio.h>
#include <math.h>
#include "mpi.h"
#include <signal.h>
#include <time.h>
#include <mpi-ext.h>
#include <stdlib.h>


int rank=MPI_PROC_NULL, verbose=0; 
char** gargv;

int MPIX_Comm_replace(MPI_Comm comm, MPI_Comm *newcomm) {
    MPI_Comm icomm,
             scomm, 
             mcomm; 
    MPI_Group cgrp, sgrp, dgrp;
    int rc, flag, rflag, i, nc, ns, nd, crank, srank, drank;

redo:
    if( comm == MPI_COMM_NULL ) {      //le nouveau processus attend son assignment de la part de rank 0
        MPI_Comm_get_parent(&icomm);
        scomm = MPI_COMM_WORLD;
        MPI_Recv(&crank, 1, MPI_INT, 0, 1, icomm, MPI_STATUS_IGNORE);
	
       if( verbose ) {
            MPI_Comm_rank(scomm, &srank);
            printf("Spawnee %d: crank=%d\n", srank, crank);
        }
    }
    
    else {  //la procedure de réparation pour les processus survivants 
      
      MPIX_Comm_shrink(comm, &scomm);    // shrinké le communicateur qui contient les echecs
      MPI_Comm_size(scomm, &ns);
      MPI_Comm_size(comm, &nc);
      nd = nc-ns; /* connaitre le nombre des processus tués  */
        if( 0 == nd ) {
            /* dans le cas ou y'a aucun processus tué , on fait rien */
            MPI_Comm_free(&scomm);
            *newcomm = comm;
            return MPI_SUCCESS;
        }
        
        MPI_Comm_set_errhandler( scomm, MPI_ERRORS_RETURN );

        rc = MPI_Comm_spawn(gargv[0], &gargv[1], nd, MPI_INFO_NULL,
                            0, scomm, &icomm, MPI_ERRCODES_IGNORE);
        flag = (MPI_SUCCESS == rc);
        MPIX_Comm_agree(scomm, &flag);
        if( !flag ) {
            if( MPI_SUCCESS == rc ) {
                MPIX_Comm_revoke(icomm);
                MPI_Comm_free(&icomm);
            }
            MPI_Comm_free(&scomm);
            if( verbose ) fprintf(stderr, "%04d: comm_spawn failed, redo\n", rank);
            goto redo;
        }

        /* se rappeller de l'ordre ancien pour le réassigner dans le nouveau communicator */
        MPI_Comm_rank(comm, &crank);
        MPI_Comm_rank(scomm, &srank);

	
        if(0 == srank) {
           
            MPI_Comm_group(comm, &cgrp);
            MPI_Comm_group(scomm, &sgrp);
            MPI_Group_difference(cgrp, sgrp, &dgrp);
            for(i=0; i<nd; i++) {
                MPI_Group_translate_ranks(dgrp, 1, &i, cgrp, &drank);
		// envoyer le nouveau assignement (rank) a le processus spawné 
                MPI_Send(&drank, 1, MPI_INT, i, 1, icomm);
            }
            MPI_Group_free(&cgrp); MPI_Group_free(&sgrp); MPI_Group_free(&dgrp);
        }
    }

    
    rc = MPI_Intercomm_merge(icomm, 1, &mcomm);
    rflag = flag = (MPI_SUCCESS==rc);
    MPIX_Comm_agree(scomm, &flag);
    if( MPI_COMM_WORLD != scomm ) MPI_Comm_free(&scomm);
    MPIX_Comm_agree(icomm, &rflag);
    MPI_Comm_free(&icomm);
    if( !(flag && rflag) ) {
        if( MPI_SUCCESS == rc ) {
            MPI_Comm_free(&mcomm);
        }
        if( verbose ) fprintf(stderr, "%04d: Intercomm_merge failed, redo\n", rank);
        goto redo;
    }

    rc = MPI_Comm_split(mcomm, 1, crank, newcomm);
    //réordre  le communicateur selon l'orignale dans World
    flag = (MPI_SUCCESS==rc);
    MPIX_Comm_agree(mcomm, &flag);
    MPI_Comm_free(&mcomm);
    if( !flag ) {
        if( MPI_SUCCESS == rc ) {
            MPI_Comm_free( newcomm );
        }
        if( verbose ) fprintf(stderr, "%04d: comm_split failed, redo\n", rank);
        goto redo;
    }

    /* restaurer l'erreur handler  */
    if( MPI_COMM_NULL != comm ) {
        MPI_Errhandler errh;
        MPI_Comm_get_errhandler( comm, &errh );
        MPI_Comm_set_errhandler( *newcomm, errh );
    }

    return MPI_SUCCESS;
}

#define maxn 12

void static_data(int first, int last, int rank, int size,double xlocal[(12/4)+2][12]) {         // Recovery schemes for static Data

  int i, j;
  
  for (i=1; i<=maxn/size; i++) 
	for (j=0; j<maxn; j++) 
	    xlocal[i][j] = rank;
  if (rank == size-1)
    for (j=0; j<maxn; j++) {
	xlocal[first][j] = -1;
    }
  else if (rank == 0)
    for (j=0; j<maxn; j++) {
	xlocal[last][j] = -1;
    }
}

/* This example handles a 12 x 12 mesh, on 4 processors only. */

int main( argc, argv )
int argc;
char **argv;
{
  int        rank, value, size, errcnt, toterr, i, j, itcnt,recov_it;
    int        i_first, i_last,rc;
    MPI_Status status;
    double     diffnorm, gdiffnorm,recov;
    double     xlocal[(12/4)+2][12];
    double     xrec[(12/4)+2][12];
    double     xnew[(12/3)+2][12];
    MPI_Comm world,rworld;
    FILE *fp;
    MPI_Request request;
    double t,ts;
    clock_t begin,end;
    double t1,t2;

    gargv = argv;
    MPI_Init( &argc, &argv );

     MPI_Comm_get_parent( &world );
     
    if( MPI_COMM_NULL == world ) {
      // on initialise notre communicateur 
      MPI_Comm_dup( MPI_COMM_WORLD, &world );
      MPI_Comm_size( world, &size );
      MPI_Comm_rank( world, &rank );
      MPI_Comm_set_errhandler( world, MPI_ERRORS_RETURN );
    } else {
      // si le communicateur existe déja et le processus est spawné , il reprend le travail
       MPIX_Comm_replace( MPI_COMM_NULL, &world );
      
      MPI_Comm_size( world, &size );
      MPI_Comm_rank( world, &rank );
      MPI_Comm_set_errhandler( world, MPI_ERRORS_RETURN );
      goto reprise;
    }

    i_first = 1;
    i_last  = maxn/size;
    if (rank == 0)        i_first++;
    if (rank == size - 1) i_last--;

    int victim;                                                                     //Un processus qui se suicide
    srand (time(NULL));
    victim = (rand()%(size-1))+ 1;
    victim = 1;
    int sz;
    t = 0;
    ts = 0;
   
    for (i=1; i<=maxn/size; i++) 
      for (j=0; j<maxn; j++) 
	xlocal[i][j] = rank;
    for (j=0; j<maxn; j++) {
      xlocal[i_first-1][j] = -1;
	xlocal[i_last+1][j] = -1;
    }

    int dead;
    int me;
    itcnt = 0;
    do {
	/* Send up unless I'm at the top, then receive from below */
	/* Note the use of xlocal[i] for &xlocal[i][0] */

      
      //  t1 = MPI_Wtime();
      
      //begin = clock();


      if((rank == victim) && (rank != 0) && (itcnt == 250)) {
      
		exit(0);
      }
      
      if (rank < size - 1) 
	MPI_Send( xlocal[maxn/size], maxn, MPI_DOUBLE, rank + 1, 0, 
		  world );
      if (rank > 0)
	MPI_Recv( xlocal[0], maxn, MPI_DOUBLE, rank - 1, 0, 
		  world, &status );
 
      
      /* Send down unless I'm at the bottom */
      if (rank > 0) 
	MPI_Send( xlocal[1], maxn, MPI_DOUBLE, rank - 1, 1, 
		  world );
      if (rank < size - 1)   
	rc = MPI_Recv( xlocal[maxn/size+1], maxn, MPI_DOUBLE, rank + 1, 1, 
		       world, &status );
      	  
      if( (MPI_ERR_PROC_FAILED == rc) || (MPI_ERR_PENDING == rc) ) {
	me = rank;
	dead = status.MPI_SOURCE;

	for(i=0;i<size;i++) {
	  if(i !=rank) {
	    MPI_Isend( &dead, 1, MPI_INT, i, 0,   world,&request);
	  }
	}
      }  


      if( (MPI_ERR_PROC_FAILED == rc) || (MPI_ERR_PENDING == rc) ) {

	rc = MPI_Irecv(&dead, 1, MPI_INT, me, 1,world,&request);
	
	MPI_Isend( &dead, 1, MPI_INT, MPI_ANY_SOURCE, 0,   world,&request);

	
      }
      
	/* Compute new values (but not on boundary) */
	itcnt ++;
	diffnorm = 0.0;
	for (i=i_first; i<=i_last; i++) 
	  for (j=1; j<maxn-1; j++) {
	    xnew[i][j] = (xlocal[i][j+1] + xlocal[i][j-1] +
			  xlocal[i+1][j] + xlocal[i-1][j]) / 4.0;
	    diffnorm += (xnew[i][j] - xlocal[i][j]) * 
	      (xnew[i][j] - xlocal[i][j]);
	  }
	/* Only transfer the interior points */
	for (i=i_first; i<=i_last; i++) 
	  for (j=1; j<maxn-1; j++) 
	    xlocal[i][j] = xnew[i][j];
       	
	
	rc = MPI_Allreduce( &diffnorm, &gdiffnorm, 1, MPI_DOUBLE, MPI_SUM,
			    world );

	if( (MPI_ERR_PROC_FAILED == rc) || (MPI_ERR_PENDING == rc) ) {
	  MPI_Irecv( &dead, 1, MPI_INT,MPI_ANY_SOURCE, 0,   world,&request);
	  MPIX_Comm_replace( world, &rworld );                      // appeller la fonction de réparation et remplacer le communicateur érroné avec le nouveau 
	  MPI_Comm_free( &world );                                  // et commencer la partie de recovery 
	  world = rworld;
        
	  goto reprise;
	}
	//   t2 = MPI_Wtime();

	
	//end = clock();
	//time_spent = (double)(end - begin) / CLOCKS_PER_SEC;
	//t = t + time_spent;
	
	gdiffnorm = sqrt( gdiffnorm );
	if (rank == 0) printf("%e %d\n",gdiffnorm, itcnt);

    } while (gdiffnorm > 1.0e-13 && itcnt < 10000);

        MPI_Finalize( );
    return 0; 
 reprise:
   
    MPI_Bcast(&dead, 1, MPI_INT, 0, world);
    victim = dead;
    
    i_first = 1;
    i_last  = maxn/size;
    if (rank == 0)        i_first++;
    if (rank == size - 1) i_last--;

    if (rank==victim)
      static_data(i_first, i_last,  rank, size,xlocal);
    

    do {
	/* Send up unless I'm at the top, then receive from below */
	/* Note the use of xlocal[i] for &xlocal[i][0] */
    
      
	if (rank < size - 1) 
	    MPI_Send( xlocal[maxn/size], maxn, MPI_DOUBLE, rank + 1, 0, 
		      world );
	if (rank > 0)
	    rc = MPI_Recv( xlocal[0], maxn, MPI_DOUBLE, rank - 1, 0, 
		      world, &status );
	if( (MPI_ERR_PROC_FAILED == rc) || (MPI_ERR_PENDING == rc) ) {
	  me = rank;
	  dead = status.MPI_SOURCE;
	  
	}
	/* Send down unless I'm at the bottom */
	if (rank > 0) 
	    MPI_Send( xlocal[1], maxn, MPI_DOUBLE, rank - 1, 1, 
		      world );
	if (rank < size - 1) 
	    rc = MPI_Recv( xlocal[maxn/size+1], maxn, MPI_DOUBLE, rank + 1, 1, 
		      world, &status );

	 if( (MPI_ERR_PROC_FAILED == rc) || (MPI_ERR_PENDING == rc) ) {
	me = rank;
	dead = status.MPI_SOURCE;

	
	for(i=0;i<size;i++) {
	  if(i !=rank) {
	    MPI_Isend( &dead, 1, MPI_INT, i, 0,   world,&request);
	  }
	}
      }  

  
      if( (MPI_ERR_PROC_FAILED == rc) || (MPI_ERR_PENDING == rc) ) {

	rc = MPI_Irecv(&dead, 1, MPI_INT, me, 1,world,&request);
	
	MPI_Isend( &dead, 1, MPI_INT, MPI_ANY_SOURCE, 0,   world,&request);

      }
      
	
	/* Compute new values (but not on boundary) */
	itcnt ++;
	diffnorm = 0;
	for (i=i_first; i<=i_last; i++) 
	    for (j=1; j<maxn-1; j++) {
		xnew[i][j] = (xlocal[i][j+1] + xlocal[i][j-1] +
			      xlocal[i+1][j] + xlocal[i-1][j]) / 4.0;
		diffnorm += (xnew[i][j] - xlocal[i][j]) * 
		            (xnew[i][j] - xlocal[i][j]);
	    }
	/* Only transfer the interior points */
	
	for (i=i_first; i<=i_last; i++) 
	    for (j=1; j<maxn-1; j++) 
		xlocal[i][j] = xnew[i][j];
	

	rc = MPI_Allreduce( &diffnorm, &gdiffnorm, 1, MPI_DOUBLE, MPI_SUM,
		       world );

	
	if( (MPI_ERR_PROC_FAILED == rc) || (MPI_ERR_PENDING == rc) ) {
	  MPI_Irecv( &dead, 1, MPI_INT,MPI_ANY_SOURCE, 0,   world,&request);
	  MPIX_Comm_replace( world, &rworld );                      // appeller la fonction de réparation et remplacer le communicateur érroné avec le nouveau 
	  MPI_Comm_free( &world );                                  // et commencer la partie de recovery 
	  world = rworld;
	  goto reprise;
	}
	


       	if ((rank == 5) && (itcnt == 200)) {

	  exit(0);
	 
	}
	
	if ((rank == 4) && (itcnt == 400)) {

	  exit(0);
	 
	}


	if ((rank == 2) && (itcnt == 600)) {

	  // exit(0);
	 
	}

	if ((rank == 3) && (itcnt == 800)) {

	  exit(0);
	
	}

	if ((rank == 4) && (itcnt == 650)) {

	  // exit(0);
	
	}
	
	gdiffnorm = sqrt( gdiffnorm );

	
	if (rank == 0) printf("%e %d\n",gdiffnorm, itcnt);

    } while (gdiffnorm > 1.0e-13 && itcnt < 10000);
    
    return 0;
}

